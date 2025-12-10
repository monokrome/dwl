#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "attached_surface.h"
#include "wlr-attached-surface-unstable-v1-protocol.h"

static struct wl_global *manager_global = NULL;
static struct wlr_scene *attached_scene = NULL;
static struct wl_list attached_surfaces;
static uint32_t serial_counter = 0;

static void attached_surface_destroy(AttachedSurface *as);

/* --- Attached Surface Implementation --- */

static void handle_set_position(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y)
{
	AttachedSurface *as = wl_resource_get_user_data(resource);
	if (!as) return;
	as->pending_x = x;
	as->pending_y = y;
}

static void handle_set_size(struct wl_client *client,
		struct wl_resource *resource, uint32_t width, uint32_t height)
{
	AttachedSurface *as = wl_resource_get_user_data(resource);
	if (!as) return;
	as->pending_width = width;
	as->pending_height = height;
}

static void handle_ack_configure(struct wl_client *client,
		struct wl_resource *resource, uint32_t serial)
{
	AttachedSurface *as = wl_resource_get_user_data(resource);
	if (!as) return;
	if (serial == as->configure_serial) {
		as->configured = 1;
	}
}

static void handle_surface_destroy(struct wl_client *client,
		struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zwlr_attached_surface_v1_interface attached_surface_impl = {
	.set_position = handle_set_position,
	.set_size = handle_set_size,
	.ack_configure = handle_ack_configure,
	.destroy = handle_surface_destroy,
};

static void attached_surface_resource_destroy(struct wl_resource *resource)
{
	AttachedSurface *as = wl_resource_get_user_data(resource);
	if (as) {
		attached_surface_destroy(as);
	}
}

static void handle_surface_commit(struct wl_listener *listener, void *data)
{
	AttachedSurface *as = wl_container_of(listener, as, surface_commit);
	struct wlr_scene_tree *parent_tree;
	int parent_x, parent_y;

	if (!as->configured) {
		return;
	}

	/* Apply pending state */
	as->x = as->pending_x;
	as->y = as->pending_y;
	as->width = as->pending_width;
	as->height = as->pending_height;

	/* Update scene node position relative to parent */
	if (as->parent && as->scene) {
		parent_tree = as->parent->base->surface->data;
		parent_x = 0;
		parent_y = 0;

		/* Get parent's position in scene coordinates */
		if (parent_tree) {
			wlr_scene_node_coords(&parent_tree->node, &parent_x, &parent_y);
		}

		wlr_scene_node_set_position(&as->scene->node, parent_x + as->x, parent_y + as->y);
		wlr_scene_node_set_enabled(&as->scene->node, 1);
		as->mapped = 1;
	}
}

static void handle_wlr_surface_destroy(struct wl_listener *listener, void *data)
{
	AttachedSurface *as = wl_container_of(listener, as, surface_destroy);
	attached_surface_destroy(as);
}

static void handle_parent_destroy(struct wl_listener *listener, void *data)
{
	AttachedSurface *as = wl_container_of(listener, as, parent_destroy);

	/* Send closed event to client */
	zwlr_attached_surface_v1_send_closed(as->resource);

	/* Clean up */
	as->parent = NULL;
	wl_list_remove(&as->parent_destroy.link);
	wl_list_init(&as->parent_destroy.link);
}

static void attached_surface_destroy(AttachedSurface *as)
{
	if (!as) return;

	wl_list_remove(&as->link);
	wl_list_remove(&as->surface_commit.link);
	wl_list_remove(&as->surface_destroy.link);
	if (as->parent) {
		wl_list_remove(&as->parent_destroy.link);
	}

	if (as->scene) {
		wlr_scene_node_destroy(&as->scene->node);
	}

	wl_resource_set_user_data(as->resource, NULL);
	free(as);
}

/* --- Manager Implementation --- */

static void manager_handle_get_attached_surface(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource,
		struct wl_resource *parent_resource)
{
	struct wlr_surface *surface;
	struct wlr_xdg_toplevel *parent;
	AttachedSurface *as;

	surface = wlr_surface_from_resource(surface_resource);
	if (!surface) {
		wl_resource_post_error(resource,
			ZWLR_ATTACHED_SURFACE_MANAGER_V1_ERROR_ROLE,
			"invalid surface");
		return;
	}

	parent = wlr_xdg_toplevel_from_resource(parent_resource);
	if (!parent) {
		wl_resource_post_error(resource,
			ZWLR_ATTACHED_SURFACE_MANAGER_V1_ERROR_INVALID_PARENT,
			"parent is not a valid xdg_toplevel");
		return;
	}

	as = calloc(1, sizeof(*as));
	if (!as) {
		wl_client_post_no_memory(client);
		return;
	}

	as->resource = wl_resource_create(client,
		&zwlr_attached_surface_v1_interface, 1, id);
	if (!as->resource) {
		free(as);
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(as->resource, &attached_surface_impl,
		as, attached_surface_resource_destroy);

	as->surface_resource = surface_resource;
	as->surface = surface;
	as->parent = parent;

	/* Create scene tree for this surface */
	as->scene = wlr_scene_subsurface_tree_create(&attached_scene->tree, surface);
	if (!as->scene) {
		wl_resource_destroy(as->resource);
		free(as);
		wl_client_post_no_memory(client);
		return;
	}
	wlr_scene_node_set_enabled(&as->scene->node, 0); /* Hidden until configured */

	/* Set up listeners */
	as->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&surface->events.commit, &as->surface_commit);

	as->surface_destroy.notify = handle_wlr_surface_destroy;
	wl_signal_add(&surface->events.destroy, &as->surface_destroy);

	as->parent_destroy.notify = handle_parent_destroy;
	wl_signal_add(&parent->events.destroy, &as->parent_destroy);

	wl_list_insert(&attached_surfaces, &as->link);

	/* Send initial configure */
	as->configure_serial = ++serial_counter;
	as->pending_width = 200;
	as->pending_height = 200;
	zwlr_attached_surface_v1_send_configure(as->resource,
		as->configure_serial, as->pending_width, as->pending_height);
}

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zwlr_attached_surface_manager_v1_interface manager_impl = {
	.get_attached_surface = manager_handle_get_attached_surface,
	.destroy = manager_handle_destroy,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwlr_attached_surface_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

/* --- Public API --- */

void attached_surface_init(struct wl_display *display, struct wlr_scene *scene)
{
	wl_list_init(&attached_surfaces);
	attached_scene = scene;

	manager_global = wl_global_create(display,
		&zwlr_attached_surface_manager_v1_interface, 1, NULL, manager_bind);
}

void attached_surface_finish(void)
{
	AttachedSurface *as, *tmp;
	wl_list_for_each_safe(as, tmp, &attached_surfaces, link) {
		attached_surface_destroy(as);
	}

	if (manager_global) {
		wl_global_destroy(manager_global);
		manager_global = NULL;
	}
}

void attached_surface_update_positions(void)
{
	AttachedSurface *as;
	struct wlr_scene_tree *parent_tree;
	int parent_x, parent_y;

	wl_list_for_each(as, &attached_surfaces, link) {
		if (!as->mapped || !as->parent || !as->scene) continue;

		parent_tree = as->parent->base->surface->data;
		if (!parent_tree) continue;

		parent_x = 0;
		parent_y = 0;
		wlr_scene_node_coords(&parent_tree->node, &parent_x, &parent_y);
		wlr_scene_node_set_position(&as->scene->node, parent_x + as->x, parent_y + as->y);
	}
}

struct wl_list *attached_surface_get_list(void)
{
	return &attached_surfaces;
}

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_output_layout.h>

#include "attached_surface.h"
#include "wlr-attached-surface-unstable-v1-protocol.h"

static struct wl_global *manager_global = NULL;
static struct wl_list attached_surfaces;
static uint32_t serial_counter = 0;
static struct wlr_output_layout *output_layout = NULL;
static struct wlr_scene_tree *overlay_layer = NULL;
static struct wlr_xdg_toplevel *focused_toplevel = NULL;

static void attached_surface_destroy(AttachedSurface *as);

/* Get parent's absolute position via its scene tree */
static int get_parent_position(AttachedSurface *as, int *px, int *py)
{
	struct wlr_scene_tree *parent_tree;

	if (!as->parent || !as->parent->base || !as->parent->base->surface)
		return 0;

	parent_tree = as->parent->base->surface->data;
	if (!parent_tree)
		return 0;

	wlr_scene_node_coords(&parent_tree->node, px, py);
	return 1;
}

/* Get the available space for an attached surface based on screen bounds */
static void get_constrained_size(AttachedSurface *as, uint32_t requested_w, uint32_t requested_h,
		uint32_t *out_w, uint32_t *out_h)
{
	struct wlr_box output_box;
	int parent_x = 0, parent_y = 0;
	int32_t available_w, available_h;

	*out_w = requested_w;
	*out_h = requested_h;

	if (!output_layout || !get_parent_position(as, &parent_x, &parent_y))
		return;

	/* Find the monitor the parent is on and constrain to that */
	struct wlr_output *output = wlr_output_layout_output_at(
		output_layout, parent_x, parent_y);
	wlr_output_layout_get_box(output_layout, output, &output_box);

	switch (as->pending_anchor) {
	case ATTACHED_ANCHOR_RIGHT:
		available_w = (output_box.x + output_box.width) - (parent_x + as->parent->current.width + as->pending_anchor_margin);
		available_h = output_box.height;
		break;
	case ATTACHED_ANCHOR_LEFT:
		available_w = parent_x - as->pending_anchor_margin - output_box.x;
		available_h = output_box.height;
		break;
	case ATTACHED_ANCHOR_TOP:
		available_w = output_box.width;
		available_h = parent_y - as->pending_anchor_margin - output_box.y;
		break;
	case ATTACHED_ANCHOR_BOTTOM:
		available_w = output_box.width;
		available_h = (output_box.y + output_box.height) - (parent_y + as->parent->current.height + as->pending_anchor_margin);
		break;
	default:
		available_w = output_box.width;
		available_h = output_box.height;
		break;
	}

	if (available_w > 0 && (uint32_t)available_w < requested_w)
		*out_w = (uint32_t)available_w;
	if (available_h > 0 && (uint32_t)available_h < requested_h)
		*out_h = (uint32_t)available_h;

	if (*out_w == 0) *out_w = 1;
	if (*out_h == 0) *out_h = 1;
}

/* Calculate absolute position based on anchor + parent position */
static void calculate_absolute_position(AttachedSurface *as, int32_t *out_x, int32_t *out_y)
{
	int parent_x = 0, parent_y = 0;
	int32_t rel_x, rel_y;

	if (!get_parent_position(as, &parent_x, &parent_y)) {
		*out_x = as->x;
		*out_y = as->y;
		return;
	}

	if (as->anchor == ATTACHED_ANCHOR_NONE) {
		*out_x = parent_x + as->x;
		*out_y = parent_y + as->y;
		return;
	}

	switch (as->anchor) {
	case ATTACHED_ANCHOR_RIGHT:
		rel_x = as->parent->current.width + as->anchor_margin;
		rel_y = as->anchor_offset;
		break;
	case ATTACHED_ANCHOR_LEFT:
		rel_x = -(int32_t)as->width - as->anchor_margin;
		rel_y = as->anchor_offset;
		break;
	case ATTACHED_ANCHOR_TOP:
		rel_x = as->anchor_offset;
		rel_y = -(int32_t)as->height - as->anchor_margin;
		break;
	case ATTACHED_ANCHOR_BOTTOM:
		rel_x = as->anchor_offset;
		rel_y = as->parent->current.height + as->anchor_margin;
		break;
	default:
		rel_x = 0;
		rel_y = 0;
		break;
	}

	*out_x = parent_x + rel_x;
	*out_y = parent_y + rel_y;
}

static void update_scene_position(AttachedSurface *as)
{
	int32_t abs_x, abs_y;

	if (!as->scene)
		return;

	calculate_absolute_position(as, &abs_x, &abs_y);
	as->x = abs_x;
	as->y = abs_y;
	wlr_scene_node_set_position(&as->scene->node, abs_x, abs_y);
}

static void handle_set_anchor(struct wl_client *client,
		struct wl_resource *resource, uint32_t anchor, int32_t margin, int32_t offset)
{
	AttachedSurface *as = wl_resource_get_user_data(resource);
	if (!as) return;

	as->pending_anchor = (enum AttachedSurfaceAnchor)anchor;
	as->pending_anchor_margin = margin;
	as->pending_anchor_offset = offset;

	if (as->mapped && as->scene) {
		as->anchor = as->pending_anchor;
		as->anchor_margin = as->pending_anchor_margin;
		as->anchor_offset = as->pending_anchor_offset;
		update_scene_position(as);
	}
}

static void handle_set_position(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y)
{
	AttachedSurface *as = wl_resource_get_user_data(resource);
	if (!as) return;
	as->pending_x = x;
	as->pending_y = y;

	if (as->mapped && as->scene && as->anchor == ATTACHED_ANCHOR_NONE)
		update_scene_position(as);
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
	if (serial == as->configure_serial)
		as->configured = 1;
}

static void handle_surface_destroy(struct wl_client *client,
		struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zwlr_attached_surface_v1_interface attached_surface_impl = {
	.set_anchor = handle_set_anchor,
	.set_position = handle_set_position,
	.set_size = handle_set_size,
	.ack_configure = handle_ack_configure,
	.destroy = handle_surface_destroy,
};

static void attached_surface_resource_destroy(struct wl_resource *resource)
{
	AttachedSurface *as = wl_resource_get_user_data(resource);
	if (as)
		attached_surface_destroy(as);
}

static void handle_surface_commit(struct wl_listener *listener, void *data)
{
	AttachedSurface *as = wl_container_of(listener, as, surface_commit);
	int is_focused;

	/* On first commit, send configure with constrained size */
	if (as->configure_serial == 0) {
		uint32_t constrained_w, constrained_h;
		get_constrained_size(as, as->pending_width, as->pending_height,
			&constrained_w, &constrained_h);
		as->configure_serial = ++serial_counter;
		zwlr_attached_surface_v1_send_configure(as->resource,
			as->configure_serial, constrained_w, constrained_h);
		return;
	}

	if (!as->configured)
		return;

	/* Apply pending state */
	as->width = as->pending_width;
	as->height = as->pending_height;
	as->anchor = as->pending_anchor;
	as->anchor_margin = as->pending_anchor_margin;
	as->anchor_offset = as->pending_anchor_offset;

	update_scene_position(as);

	if (as->scene) {
		as->mapped = 1;
		/* Only show if parent is currently focused */
		is_focused = as->parent && as->parent == focused_toplevel;
		wlr_scene_node_set_enabled(&as->scene->node, is_focused);
	}
}

static void handle_scene_destroy(struct wl_listener *listener, void *data)
{
	AttachedSurface *as = wl_container_of(listener, as, scene_destroy);
	as->scene = NULL;
	wl_list_remove(&as->scene_destroy.link);
	wl_list_init(&as->scene_destroy.link);
}

static void handle_wlr_surface_destroy(struct wl_listener *listener, void *data)
{
	AttachedSurface *as = wl_container_of(listener, as, surface_destroy);
	attached_surface_destroy(as);
}

static void handle_parent_destroy(struct wl_listener *listener, void *data)
{
	AttachedSurface *as = wl_container_of(listener, as, parent_destroy);

	zwlr_attached_surface_v1_send_closed(as->resource);

	/* Scene is in overlay layer (not parented to parent's tree), so we
	 * need to hide it but don't need to NULL it — cleanup in destroy. */
	if (as->scene)
		wlr_scene_node_set_enabled(&as->scene->node, 0);

	as->parent = NULL;
	as->mapped = 0;
	wl_list_remove(&as->parent_destroy.link);
	wl_list_init(&as->parent_destroy.link);
}

static void attached_surface_destroy(AttachedSurface *as)
{
	if (!as) return;

	wl_list_remove(&as->link);
	wl_list_remove(&as->surface_commit.link);
	wl_list_remove(&as->surface_destroy.link);
	if (as->parent)
		wl_list_remove(&as->parent_destroy.link);

	if (as->scene) {
		wl_list_remove(&as->scene_destroy.link);
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

	if (!parent_resource) {
		wl_resource_post_error(resource,
			ZWLR_ATTACHED_SURFACE_MANAGER_V1_ERROR_INVALID_PARENT,
			"parent resource is NULL");
		return;
	}

	parent = wlr_xdg_toplevel_from_resource(parent_resource);
	if (!parent) {
		wl_resource_post_error(resource,
			ZWLR_ATTACHED_SURFACE_MANAGER_V1_ERROR_INVALID_PARENT,
			"parent xdg_toplevel is inert (destroyed?)");
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

	/* Verify the surface doesn't already have a role */
	if (surface->role) {
		wl_resource_post_error(resource,
			ZWLR_ATTACHED_SURFACE_MANAGER_V1_ERROR_ROLE,
			"surface already has a role");
		wl_resource_set_user_data(as->resource, NULL);
		free(as);
		return;
	}

	/* Create scene tree in the overlay layer (above normal windows) */
	as->scene = wlr_scene_subsurface_tree_create(overlay_layer, surface);
	if (!as->scene) {
		wl_resource_set_user_data(as->resource, NULL);
		wl_resource_destroy(as->resource);
		free(as);
		wl_client_post_no_memory(client);
		return;
	}
	wlr_scene_node_set_enabled(&as->scene->node, 0);

	/* Set up listeners */
	as->scene_destroy.notify = handle_scene_destroy;
	wl_signal_add(&as->scene->node.events.destroy, &as->scene_destroy);

	as->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&surface->events.commit, &as->surface_commit);

	as->surface_destroy.notify = handle_wlr_surface_destroy;
	wl_signal_add(&surface->events.destroy, &as->surface_destroy);

	as->parent_destroy.notify = handle_parent_destroy;
	wl_signal_add(&parent->events.destroy, &as->parent_destroy);

	wl_list_insert(&attached_surfaces, &as->link);

	as->pending_width = 0;
	as->pending_height = 0;
	as->configured = 0;
	as->configure_serial = 0;
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

void attached_surface_init(struct wl_display *display, struct wlr_output_layout *layout,
		struct wlr_scene_tree *layer)
{
	output_layout = layout;
	overlay_layer = layer;
	wl_list_init(&attached_surfaces);

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
	int32_t abs_x, abs_y;

	wl_list_for_each(as, &attached_surfaces, link) {
		if (!as->mapped || !as->scene || !as->parent)
			continue;

		calculate_absolute_position(as, &abs_x, &abs_y);
		if (abs_x != as->x || abs_y != as->y) {
			as->x = abs_x;
			as->y = abs_y;
			wlr_scene_node_set_position(&as->scene->node, abs_x, abs_y);
		}
	}
}

void attached_surface_set_focus(struct wlr_xdg_toplevel *focused)
{
	AttachedSurface *as;
	int show;

	focused_toplevel = focused;

	wl_list_for_each(as, &attached_surfaces, link) {
		if (!as->mapped || !as->scene)
			continue;

		show = as->parent && as->parent == focused;
		wlr_scene_node_set_enabled(&as->scene->node, show);
	}
}

struct wl_list *attached_surface_get_list(void)
{
	return &attached_surfaces;
}

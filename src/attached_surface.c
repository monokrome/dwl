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

static void attached_surface_destroy(AttachedSurface *as);

/* --- Attached Surface Implementation --- */

/* Get the available space for an attached surface based on screen bounds */
static void get_constrained_size(AttachedSurface *as, uint32_t requested_w, uint32_t requested_h,
		uint32_t *out_w, uint32_t *out_h)
{
	struct wlr_box output_box;
	int parent_x = 0, parent_y = 0;
	int32_t available_w, available_h;

	*out_w = requested_w;
	*out_h = requested_h;

	if (!output_layout || !as->parent)
		return;

	/* Get parent's absolute position */
	struct wlr_scene_tree *parent_tree = as->parent->base->surface->data;
	if (parent_tree) {
		wlr_scene_node_coords(&parent_tree->node, &parent_x, &parent_y);
	}

	/* Get total output layout bounds */
	wlr_output_layout_get_box(output_layout, NULL, &output_box);

	/* Calculate available space based on anchor */
	switch (as->pending_anchor) {
	case ATTACHED_ANCHOR_RIGHT:
		/* Space from right edge of parent to right edge of screen */
		available_w = (output_box.x + output_box.width) - (parent_x + as->parent->current.width + as->pending_anchor_margin);
		available_h = output_box.height;
		break;
	case ATTACHED_ANCHOR_LEFT:
		/* Space from left edge of screen to left edge of parent */
		available_w = parent_x - as->pending_anchor_margin - output_box.x;
		available_h = output_box.height;
		break;
	case ATTACHED_ANCHOR_TOP:
		/* Space from top edge of screen to top edge of parent */
		available_w = output_box.width;
		available_h = parent_y - as->pending_anchor_margin - output_box.y;
		break;
	case ATTACHED_ANCHOR_BOTTOM:
		/* Space from bottom edge of parent to bottom edge of screen */
		available_w = output_box.width;
		available_h = (output_box.y + output_box.height) - (parent_y + as->parent->current.height + as->pending_anchor_margin);
		break;
	default:
		/* No anchor - just use full screen bounds for now */
		available_w = output_box.width;
		available_h = output_box.height;
		break;
	}

	/* Constrain to available space (but don't go below 1) */
	if (available_w > 0 && (uint32_t)available_w < requested_w)
		*out_w = (uint32_t)available_w;
	if (available_h > 0 && (uint32_t)available_h < requested_h)
		*out_h = (uint32_t)available_h;

	/* Minimum size of 1x1 */
	if (*out_w == 0) *out_w = 1;
	if (*out_h == 0) *out_h = 1;
}

/* Calculate position based on anchor, parent geometry, and surface size */
static void calculate_anchored_position(AttachedSurface *as, int32_t *out_x, int32_t *out_y)
{
	int32_t x = 0, y = 0;
	int32_t parent_width, parent_height;

	if (!as->parent || as->anchor == ATTACHED_ANCHOR_NONE) {
		*out_x = as->x;
		*out_y = as->y;
		return;
	}

	/* Get parent geometry from toplevel current state */
	parent_width = as->parent->current.width;
	parent_height = as->parent->current.height;

	switch (as->anchor) {
	case ATTACHED_ANCHOR_RIGHT:
		x = parent_width + as->anchor_margin;
		y = as->anchor_offset;
		break;
	case ATTACHED_ANCHOR_LEFT:
		x = -(int32_t)as->width - as->anchor_margin;
		y = as->anchor_offset;
		break;
	case ATTACHED_ANCHOR_TOP:
		x = as->anchor_offset;
		y = -(int32_t)as->height - as->anchor_margin;
		break;
	case ATTACHED_ANCHOR_BOTTOM:
		x = as->anchor_offset;
		y = parent_height + as->anchor_margin;
		break;
	default:
		x = as->x;
		y = as->y;
		break;
	}

	*out_x = x;
	*out_y = y;
}

static void handle_set_anchor(struct wl_client *client,
		struct wl_resource *resource, uint32_t anchor, int32_t margin, int32_t offset)
{
	AttachedSurface *as = wl_resource_get_user_data(resource);
	if (!as) return;

	as->pending_anchor = (enum AttachedSurfaceAnchor)anchor;
	as->pending_anchor_margin = margin;
	as->pending_anchor_offset = offset;

	/* Apply immediately if already mapped */
	if (as->mapped && as->scene) {
		int32_t x, y;
		as->anchor = as->pending_anchor;
		as->anchor_margin = as->pending_anchor_margin;
		as->anchor_offset = as->pending_anchor_offset;
		calculate_anchored_position(as, &x, &y);
		as->x = x;
		as->y = y;
		wlr_scene_node_set_position(&as->scene->node, x, y);
	}
}

static void handle_set_position(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y)
{
	AttachedSurface *as = wl_resource_get_user_data(resource);
	if (!as) return;
	as->pending_x = x;
	as->pending_y = y;

	/* Apply position immediately if already mapped and not anchored */
	if (as->mapped && as->scene && as->anchor == ATTACHED_ANCHOR_NONE) {
		as->x = x;
		as->y = y;
		wlr_scene_node_set_position(&as->scene->node, x, y);
	}
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
	.set_anchor = handle_set_anchor,
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
	int32_t x, y;

	/* On first commit (before configured), send configure with constrained size */
	if (as->configure_serial == 0) {
		uint32_t constrained_w, constrained_h;
		get_constrained_size(as, as->pending_width, as->pending_height,
			&constrained_w, &constrained_h);
		as->configure_serial = ++serial_counter;
		zwlr_attached_surface_v1_send_configure(as->resource,
			as->configure_serial, constrained_w, constrained_h);
		return;
	}

	if (!as->configured) {
		return;
	}

	/* Apply pending state */
	as->x = as->pending_x;
	as->y = as->pending_y;
	as->width = as->pending_width;
	as->height = as->pending_height;
	as->anchor = as->pending_anchor;
	as->anchor_margin = as->pending_anchor_margin;
	as->anchor_offset = as->pending_anchor_offset;

	/* Calculate position (anchored or manual) */
	calculate_anchored_position(as, &x, &y);
	as->x = x;
	as->y = y;

	/* Update scene node position relative to parent (scene is already parented) */
	if (as->scene) {
		wlr_scene_node_set_position(&as->scene->node, as->x, as->y);
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

	/* Scene node is a child of parent's tree, so it's already being destroyed.
	 * Set to NULL so we don't try to destroy it again in attached_surface_destroy. */
	as->scene = NULL;
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

	/* Create scene tree as child of parent's scene tree so it moves with parent */
	struct wlr_scene_tree *parent_tree = parent->base->surface->data;
	if (!parent_tree) {
		wl_resource_post_error(resource,
			ZWLR_ATTACHED_SURFACE_MANAGER_V1_ERROR_INVALID_PARENT,
			"parent has no scene tree");
		free(as);
		return;
	}

	as->scene = wlr_scene_subsurface_tree_create(parent_tree, surface);
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

	/* Don't send configure here - wait for initial commit so client
	 * has a chance to call set_size first. We'll send configure in
	 * handle_surface_commit when we see the first commit. */
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

void attached_surface_init(struct wl_display *display, struct wlr_output_layout *layout)
{
	output_layout = layout;
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
	int32_t x, y;

	/* Recalculate positions for anchored surfaces when parent resizes */
	wl_list_for_each(as, &attached_surfaces, link) {
		if (!as->mapped || !as->scene || as->anchor == ATTACHED_ANCHOR_NONE)
			continue;

		calculate_anchored_position(as, &x, &y);
		if (x != as->x || y != as->y) {
			as->x = x;
			as->y = y;
			wlr_scene_node_set_position(&as->scene->node, x, y);
		}
	}
}

struct wl_list *attached_surface_get_list(void)
{
	return &attached_surfaces;
}

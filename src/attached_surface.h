#ifndef ATTACHED_SURFACE_H
#define ATTACHED_SURFACE_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

struct wlr_xdg_toplevel;

typedef struct AttachedSurface AttachedSurface;

struct AttachedSurface {
	struct wl_resource *resource;
	struct wl_resource *surface_resource;
	struct wlr_surface *surface;
	struct wlr_xdg_toplevel *parent;
	struct wlr_scene_tree *scene;

	int32_t x, y;
	uint32_t width, height;

	/* Pending state (double-buffered) */
	int32_t pending_x, pending_y;
	uint32_t pending_width, pending_height;

	uint32_t configure_serial;
	int configured;
	int mapped;

	struct wl_listener surface_commit;
	struct wl_listener surface_destroy;
	struct wl_listener parent_destroy;

	struct wl_list link; /* attached_surfaces list */
};

/* Initialize the attached surface manager global */
void attached_surface_init(struct wl_display *display, struct wlr_scene *scene);

/* Clean up */
void attached_surface_finish(void);

/* Update all attached surface positions (call after parent moves) */
void attached_surface_update_positions(void);

/* Get list of all attached surfaces */
struct wl_list *attached_surface_get_list(void);

#endif /* ATTACHED_SURFACE_H */

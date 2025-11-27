/* wallpaper.h - wallpaper slideshow support for dwl */
#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <wlr/types/wlr_scene.h>
#include <wlr/render/wlr_renderer.h>

/* Initialize wallpaper system, scan directories */
void wallpaper_init(struct wlr_scene *scene, struct wlr_renderer *renderer,
		const char *dir, int interval);

/* Clean up wallpaper resources */
void wallpaper_cleanup(void);

/* Load next image in current directory */
void wallpaper_next_image(void);

/* Switch to next directory and load random image from it */
void wallpaper_next_dir(void);

/* Switch to previous directory and load random image from it */
void wallpaper_prev_dir(void);

/* Timer callback for slideshow */
int wallpaper_timer_callback(void *data);

/* Set the event loop for timer */
void wallpaper_set_event_loop(struct wl_event_loop *loop);

/* Resize wallpaper to fit output dimensions */
void wallpaper_resize(int width, int height);

#endif /* WALLPAPER_H */

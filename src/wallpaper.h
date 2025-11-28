/* wallpaper.h - wallpaper slideshow support for dwl */
#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <wlr/types/wlr_scene.h>
#include <wlr/render/wlr_renderer.h>

/* Scaling modes for config */
enum {
	WallpaperTile,    /* Tile image without scaling */
	WallpaperCenter,  /* Center without scaling */
	WallpaperFit,     /* Fit inside screen (may have borders) */
	WallpaperCover,   /* Cover screen (may crop) */
};

/* Initialize wallpaper system */
void wallpaper_init(struct wlr_scene *scene, struct wlr_renderer *renderer,
		const char *dir, int interval, int scale_mode);

/* Clean up wallpaper resources */
void wallpaper_cleanup(void);

/* Load next random image in current directory */
void wallpaper_next_image(void);

/* Switch to a random directory and load random image from it */
void wallpaper_next_dir(void);

/* Switch to another random directory (same as next_dir with random selection) */
void wallpaper_prev_dir(void);

/* Timer callback for slideshow */
int wallpaper_timer_callback(void *data);

/* Set the event loop for timer */
void wallpaper_set_event_loop(struct wl_event_loop *loop);

/* Resize wallpaper to fit output dimensions */
void wallpaper_resize(int width, int height);

#endif /* WALLPAPER_H */

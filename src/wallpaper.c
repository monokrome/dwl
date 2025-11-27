/* wallpaper.c - wallpaper slideshow support for dwl */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <wordexp.h>
#include <wayland-server-core.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <drm_fourcc.h>

#include "wallpaper.h"

#define MAX_DIRS 64
#define MAX_IMAGES 256
#define MAX_PATH 4096

/* Wallpaper buffer for wlr_scene */
typedef struct {
	struct wlr_buffer base;
	void *data;
	uint32_t format;
	size_t stride;
} WallpaperBuffer;

static struct {
	char *dirs[MAX_DIRS];
	int dir_count;
	int current_dir;

	char *images[MAX_IMAGES];
	int image_count;
	int current_image;

	struct wlr_scene_buffer *scene_buffer;
	struct wlr_scene *scene;
	struct wlr_renderer *renderer;
	WallpaperBuffer *buffer;

	struct wl_event_source *timer;
	struct wl_event_loop *event_loop;

	int width;
	int height;
	int interval;

	char base_path[MAX_PATH];
} wp;

/* Forward declarations */
static void load_current_image(void);
static void scan_directories(const char *path);
static void scan_images(const char *dir_path);
static void free_images(void);
static void free_dirs(void);
static char *expand_path(const char *path);

/* Buffer implementation */
static void buffer_destroy(struct wlr_buffer *wlr_buffer) {
	WallpaperBuffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	if (buffer->data)
		free(buffer->data);
	free(buffer);
}

static bool buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	WallpaperBuffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	*data = buffer->data;
	*format = buffer->format;
	*stride = buffer->stride;
	return true;
}

static void buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	/* Nothing to do */
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.begin_data_ptr_access = buffer_begin_data_ptr_access,
	.end_data_ptr_access = buffer_end_data_ptr_access,
};

static char *expand_path(const char *path) {
	wordexp_t exp;
	char *result = NULL;

	if (wordexp(path, &exp, WRDE_NOCMD) == 0) {
		if (exp.we_wordc > 0) {
			result = strdup(exp.we_wordv[0]);
		}
		wordfree(&exp);
	}
	return result;
}

static int cmp_str(const void *a, const void *b) {
	return strcmp(*(const char **)a, *(const char **)b);
}

static void scan_directories(const char *path) {
	DIR *dir, *test;
	struct dirent *entry;
	char full_path[MAX_PATH];

	free_dirs();

	dir = opendir(path);
	if (!dir) {
		fprintf(stderr, "wallpaper: cannot open directory %s\n", path);
		return;
	}

	while ((entry = readdir(dir)) != NULL && wp.dir_count < MAX_DIRS) {
		if (entry->d_name[0] == '.')
			continue;

		snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

		/* Check if it's a directory */
		test = opendir(full_path);
		if (test) {
			closedir(test);
			wp.dirs[wp.dir_count++] = strdup(full_path);
		}
	}
	closedir(dir);

	/* Sort directories for consistent ordering */
	if (wp.dir_count > 0)
		qsort(wp.dirs, wp.dir_count, sizeof(char *), cmp_str);
}

static int is_image_file(const char *name) {
	const char *ext = strrchr(name, '.');
	if (!ext)
		return 0;
	ext++;
	return (strcasecmp(ext, "jpg") == 0 ||
	        strcasecmp(ext, "jpeg") == 0 ||
	        strcasecmp(ext, "png") == 0 ||
	        strcasecmp(ext, "bmp") == 0 ||
	        strcasecmp(ext, "gif") == 0);
}

static void scan_images(const char *dir_path) {
	DIR *dir;
	struct dirent *entry;

	free_images();

	dir = opendir(dir_path);
	if (!dir) {
		fprintf(stderr, "wallpaper: cannot open directory %s\n", dir_path);
		return;
	}

	while ((entry = readdir(dir)) != NULL && wp.image_count < MAX_IMAGES) {
		if (entry->d_name[0] == '.')
			continue;

		if (is_image_file(entry->d_name)) {
			char full_path[MAX_PATH];
			snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
			wp.images[wp.image_count++] = strdup(full_path);
		}
	}
	closedir(dir);

	/* Shuffle images */
	if (wp.image_count > 1) {
		for (int i = wp.image_count - 1; i > 0; i--) {
			int j = rand() % (i + 1);
			char *tmp = wp.images[i];
			wp.images[i] = wp.images[j];
			wp.images[j] = tmp;
		}
	}
}

static void free_images(void) {
	for (int i = 0; i < wp.image_count; i++) {
		free(wp.images[i]);
		wp.images[i] = NULL;
	}
	wp.image_count = 0;
	wp.current_image = 0;
}

static void free_dirs(void) {
	for (int i = 0; i < wp.dir_count; i++) {
		free(wp.dirs[i]);
		wp.dirs[i] = NULL;
	}
	wp.dir_count = 0;
	wp.current_dir = 0;
}

static void load_current_image(void) {
	const char *path;
	int img_w, img_h, channels;
	unsigned char *img_data, *final_data;
	float scale_x, scale_y, scale;
	int scaled_w, scaled_h, offset_x, offset_y;
	size_t stride;
	int x, y, src_x, src_y;
	unsigned char *src, *dst;
	WallpaperBuffer *buffer;

	if (wp.image_count == 0 || wp.width == 0 || wp.height == 0)
		return;

	path = wp.images[wp.current_image];

	img_data = stbi_load(path, &img_w, &img_h, &channels, 4);
	if (!img_data) {
		fprintf(stderr, "wallpaper: failed to load %s\n", path);
		return;
	}

	/* Scale image to fit screen while maintaining aspect ratio */
	scale_x = (float)wp.width / img_w;
	scale_y = (float)wp.height / img_h;
	scale = (scale_x > scale_y) ? scale_x : scale_y; /* cover */

	scaled_w = (int)(img_w * scale);
	scaled_h = (int)(img_h * scale);
	offset_x = (wp.width - scaled_w) / 2;
	offset_y = (wp.height - scaled_h) / 2;

	/* Allocate buffer for final image */
	stride = wp.width * 4;
	final_data = calloc(1, stride * wp.height);
	if (!final_data) {
		stbi_image_free(img_data);
		return;
	}

	/* Simple nearest-neighbor scaling and centering */
	for (y = 0; y < wp.height; y++) {
		for (x = 0; x < wp.width; x++) {
			src_x = (x - offset_x) * img_w / scaled_w;
			src_y = (y - offset_y) * img_h / scaled_h;

			if (src_x >= 0 && src_x < img_w && src_y >= 0 && src_y < img_h) {
				src = img_data + (src_y * img_w + src_x) * 4;
				dst = final_data + (y * wp.width + x) * 4;
				/* RGBA to BGRA */
				dst[0] = src[2]; /* B */
				dst[1] = src[1]; /* G */
				dst[2] = src[0]; /* R */
				dst[3] = src[3]; /* A */
			}
		}
	}

	stbi_image_free(img_data);

	/* Create new buffer */
	buffer = calloc(1, sizeof(WallpaperBuffer));
	if (!buffer) {
		free(final_data);
		return;
	}

	wlr_buffer_init(&buffer->base, &buffer_impl, wp.width, wp.height);
	buffer->data = final_data;
	buffer->format = DRM_FORMAT_ARGB8888;
	buffer->stride = stride;

	/* Update scene buffer */
	if (wp.scene_buffer) {
		wlr_scene_buffer_set_buffer(wp.scene_buffer, &buffer->base);
		wlr_scene_buffer_set_dest_size(wp.scene_buffer, wp.width, wp.height);
	}

	/* Release old buffer */
	if (wp.buffer)
		wlr_buffer_drop(&wp.buffer->base);
	wp.buffer = buffer;
}

void wallpaper_init(struct wlr_scene *scene, struct wlr_renderer *renderer,
		const char *dir, int interval) {
	char *expanded;

	srand(time(NULL));

	memset(&wp, 0, sizeof(wp));
	wp.scene = scene;
	wp.renderer = renderer;
	wp.interval = interval;

	/* Expand path */
	expanded = expand_path(dir);
	if (!expanded) {
		fprintf(stderr, "wallpaper: failed to expand path %s\n", dir);
		return;
	}
	strncpy(wp.base_path, expanded, MAX_PATH - 1);
	free(expanded);

	/* Scan for directories */
	scan_directories(wp.base_path);

	if (wp.dir_count == 0) {
		fprintf(stderr, "wallpaper: no directories found in %s\n", wp.base_path);
		return;
	}

	/* Pick random starting directory */
	wp.current_dir = rand() % wp.dir_count;

	/* Scan images in that directory */
	scan_images(wp.dirs[wp.current_dir]);

	/* Create scene buffer (will be positioned at root) */
	wp.scene_buffer = wlr_scene_buffer_create(&scene->tree, NULL);
	if (wp.scene_buffer) {
		/* Place at very bottom */
		wlr_scene_node_lower_to_bottom(&wp.scene_buffer->node);
	}
}

void wallpaper_set_event_loop(struct wl_event_loop *loop) {
	wp.event_loop = loop;

	if (wp.interval > 0 && loop) {
		wp.timer = wl_event_loop_add_timer(loop, wallpaper_timer_callback, NULL);
		if (wp.timer) {
			wl_event_source_timer_update(wp.timer, wp.interval * 1000);
		}
	}
}

void wallpaper_cleanup(void) {
	if (wp.timer) {
		wl_event_source_remove(wp.timer);
		wp.timer = NULL;
	}

	if (wp.buffer) {
		wlr_buffer_drop(&wp.buffer->base);
		wp.buffer = NULL;
	}

	free_images();
	free_dirs();
}

void wallpaper_next_image(void) {
	if (wp.image_count == 0)
		return;

	wp.current_image = (wp.current_image + 1) % wp.image_count;
	load_current_image();
}

void wallpaper_next_dir(void) {
	if (wp.dir_count == 0)
		return;

	wp.current_dir = (wp.current_dir + 1) % wp.dir_count;
	scan_images(wp.dirs[wp.current_dir]);

	if (wp.image_count > 0) {
		wp.current_image = 0;
		load_current_image();
	}

	fprintf(stderr, "wallpaper: switched to %s\n", wp.dirs[wp.current_dir]);
}

void wallpaper_prev_dir(void) {
	if (wp.dir_count == 0)
		return;

	wp.current_dir = (wp.current_dir - 1 + wp.dir_count) % wp.dir_count;
	scan_images(wp.dirs[wp.current_dir]);

	if (wp.image_count > 0) {
		wp.current_image = 0;
		load_current_image();
	}

	fprintf(stderr, "wallpaper: switched to %s\n", wp.dirs[wp.current_dir]);
}

int wallpaper_timer_callback(void *data) {
	wallpaper_next_image();

	/* Reschedule timer */
	if (wp.timer && wp.interval > 0) {
		wl_event_source_timer_update(wp.timer, wp.interval * 1000);
	}

	return 0;
}

void wallpaper_resize(int width, int height) {
	if (width == wp.width && height == wp.height)
		return;

	wp.width = width;
	wp.height = height;

	if (wp.image_count > 0)
		load_current_image();
}

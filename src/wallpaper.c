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

#define MAX_PATH 4096

/* Scaling modes */
enum {
	SCALE_TILE,    /* Tile image without scaling */
	SCALE_CENTER,  /* Center without scaling */
	SCALE_FIT,     /* Fit inside screen (may have borders) */
	SCALE_COVER,   /* Cover screen (may crop) */
};

/* Wallpaper buffer for wlr_scene */
typedef struct {
	struct wlr_buffer base;
	void *data;
	uint32_t format;
	size_t stride;
} WallpaperBuffer;

static struct {
	struct wlr_scene_buffer *scene_buffer;
	struct wlr_scene *scene;
	struct wlr_renderer *renderer;
	WallpaperBuffer *buffer;

	struct wl_event_source *timer;
	struct wl_event_loop *event_loop;

	int width;
	int height;
	int interval;
	int scale_mode;

	char base_path[MAX_PATH];
	char current_dir[MAX_PATH];
	char current_file[MAX_PATH];
} wp;

/* Forward declarations */
static void load_random_image(void);
static void read_scale_mode(const char *dir_path);
static char *expand_path(const char *path);
static char *pick_random_subdir(const char *path);
static char *pick_random_image(const char *dir_path);

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

static int is_directory(const char *path) {
	DIR *dir = opendir(path);
	if (dir) {
		closedir(dir);
		return 1;
	}
	return 0;
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

/* Pick a random subdirectory from path, returns allocated string or NULL */
static char *pick_random_subdir(const char *path) {
	DIR *dir;
	struct dirent *entry;
	char **dirs = NULL;
	int count = 0;
	int capacity = 0;
	char *result = NULL;
	char full_path[MAX_PATH];

	dir = opendir(path);
	if (!dir)
		return NULL;

	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;

		snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

		if (is_directory(full_path)) {
			if (count >= capacity) {
				capacity = capacity ? capacity * 2 : 16;
				dirs = realloc(dirs, capacity * sizeof(char *));
			}
			dirs[count++] = strdup(full_path);
		}
	}
	closedir(dir);

	if (count > 0) {
		int idx = rand() % count;
		result = dirs[idx];
		dirs[idx] = NULL; /* Don't free the one we're returning */
	}

	/* Free the rest */
	for (int i = 0; i < count; i++) {
		if (dirs[i])
			free(dirs[i]);
	}
	free(dirs);

	return result;
}

/* Pick a random image from dir_path, returns allocated string or NULL */
static char *pick_random_image(const char *dir_path) {
	DIR *dir;
	struct dirent *entry;
	char **images = NULL;
	int count = 0;
	int capacity = 0;
	char *result = NULL;
	char full_path[MAX_PATH];

	dir = opendir(dir_path);
	if (!dir)
		return NULL;

	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;

		if (is_image_file(entry->d_name)) {
			snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
			if (count >= capacity) {
				capacity = capacity ? capacity * 2 : 16;
				images = realloc(images, capacity * sizeof(char *));
			}
			images[count++] = strdup(full_path);
		}
	}
	closedir(dir);

	if (count > 0) {
		int idx = rand() % count;
		result = images[idx];
		images[idx] = NULL;
	}

	for (int i = 0; i < count; i++) {
		if (images[i])
			free(images[i]);
	}
	free(images);

	return result;
}

static void read_scale_mode(const char *dir_path) {
	char path[MAX_PATH];
	FILE *f;
	char buf[32];
	size_t len;

	snprintf(path, sizeof(path), "%s/.scaling", dir_path);
	f = fopen(path, "r");
	if (!f)
		return;

	if (fgets(buf, sizeof(buf), f)) {
		len = strlen(buf);
		if (len > 0 && buf[len - 1] == '\n')
			buf[len - 1] = '\0';

		if (strcasecmp(buf, "tile") == 0)
			wp.scale_mode = SCALE_TILE;
		else if (strcasecmp(buf, "center") == 0)
			wp.scale_mode = SCALE_CENTER;
		else if (strcasecmp(buf, "fit") == 0)
			wp.scale_mode = SCALE_FIT;
		else if (strcasecmp(buf, "cover") == 0)
			wp.scale_mode = SCALE_COVER;
	}

	fclose(f);
}

static void load_image_file(const char *path) {
	int img_w, img_h, channels;
	unsigned char *img_data, *final_data;
	size_t stride;
	int x, y;
	WallpaperBuffer *buffer;

	if (wp.width == 0 || wp.height == 0)
		return;

	img_data = stbi_load(path, &img_w, &img_h, &channels, 4);
	if (!img_data) {
		fprintf(stderr, "wallpaper: failed to load %s\n", path);
		return;
	}

	stride = wp.width * 4;
	final_data = calloc(1, stride * wp.height);
	if (!final_data) {
		stbi_image_free(img_data);
		return;
	}

	if (wp.scale_mode == SCALE_TILE) {
		/* Tile: repeat image across screen */
		for (y = 0; y < wp.height; y++) {
			for (x = 0; x < wp.width; x++) {
				int src_x = x % img_w;
				int src_y = y % img_h;
				unsigned char *src = img_data + (src_y * img_w + src_x) * 4;
				unsigned char *dst = final_data + (y * wp.width + x) * 4;
				dst[0] = src[2]; /* B */
				dst[1] = src[1]; /* G */
				dst[2] = src[0]; /* R */
				dst[3] = src[3]; /* A */
			}
		}
	} else {
		/* For center, fit, cover - calculate scaling */
		float scale_x = (float)wp.width / img_w;
		float scale_y = (float)wp.height / img_h;
		float scale;
		int scaled_w, scaled_h, offset_x, offset_y;

		if (wp.scale_mode == SCALE_CENTER) {
			scale = 1.0f; /* No scaling */
		} else if (wp.scale_mode == SCALE_FIT) {
			scale = (scale_x < scale_y) ? scale_x : scale_y;
		} else { /* SCALE_COVER */
			scale = (scale_x > scale_y) ? scale_x : scale_y;
		}

		scaled_w = (int)(img_w * scale);
		scaled_h = (int)(img_h * scale);
		offset_x = (wp.width - scaled_w) / 2;
		offset_y = (wp.height - scaled_h) / 2;

		for (y = 0; y < wp.height; y++) {
			for (x = 0; x < wp.width; x++) {
				int src_x = (x - offset_x) * img_w / scaled_w;
				int src_y = (y - offset_y) * img_h / scaled_h;

				if (src_x >= 0 && src_x < img_w && src_y >= 0 && src_y < img_h) {
					unsigned char *src = img_data + (src_y * img_w + src_x) * 4;
					unsigned char *dst = final_data + (y * wp.width + x) * 4;
					dst[0] = src[2]; /* B */
					dst[1] = src[1]; /* G */
					dst[2] = src[0]; /* R */
					dst[3] = src[3]; /* A */
				}
			}
		}
	}

	stbi_image_free(img_data);

	buffer = calloc(1, sizeof(WallpaperBuffer));
	if (!buffer) {
		free(final_data);
		return;
	}

	wlr_buffer_init(&buffer->base, &buffer_impl, wp.width, wp.height);
	buffer->data = final_data;
	buffer->format = DRM_FORMAT_ARGB8888;
	buffer->stride = stride;

	if (wp.scene_buffer) {
		wlr_scene_buffer_set_buffer(wp.scene_buffer, &buffer->base);
		wlr_scene_buffer_set_dest_size(wp.scene_buffer, wp.width, wp.height);
	}

	if (wp.buffer)
		wlr_buffer_drop(&wp.buffer->base);
	wp.buffer = buffer;

	strncpy(wp.current_file, path, MAX_PATH - 1);
}

static void load_random_image(void) {
	char *subdir, *image;

	/* If we don't have a current directory, pick one */
	if (wp.current_dir[0] == '\0') {
		subdir = pick_random_subdir(wp.base_path);
		if (!subdir)
			return;
		strncpy(wp.current_dir, subdir, MAX_PATH - 1);
		free(subdir);
		read_scale_mode(wp.current_dir);
	}

	image = pick_random_image(wp.current_dir);
	if (!image) {
		/* No images in current dir, try picking a new subdir */
		subdir = pick_random_subdir(wp.base_path);
		if (subdir) {
			strncpy(wp.current_dir, subdir, MAX_PATH - 1);
			free(subdir);
			read_scale_mode(wp.current_dir);
			image = pick_random_image(wp.current_dir);
		}
	}

	if (image) {
		load_image_file(image);
		free(image);
	}
}

void wallpaper_init(struct wlr_scene *scene, struct wlr_renderer *renderer,
		const char *dir, int interval, int scale_mode) {
	char *expanded;

	srand(time(NULL));

	memset(&wp, 0, sizeof(wp));
	wp.scene = scene;
	wp.renderer = renderer;
	wp.interval = interval;
	wp.scale_mode = scale_mode;

	expanded = expand_path(dir);
	if (!expanded) {
		fprintf(stderr, "wallpaper: failed to expand path %s\n", dir);
		return;
	}
	strncpy(wp.base_path, expanded, MAX_PATH - 1);
	free(expanded);

	if (!is_directory(wp.base_path)) {
		fprintf(stderr, "wallpaper: directory does not exist: %s\n", wp.base_path);
		return;
	}

	/* Create scene buffer */
	wp.scene_buffer = wlr_scene_buffer_create(&scene->tree, NULL);
	if (wp.scene_buffer) {
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
}

void wallpaper_next_image(void) {
	load_random_image();
}

void wallpaper_next_dir(void) {
	char *subdir = pick_random_subdir(wp.base_path);
	if (subdir) {
		strncpy(wp.current_dir, subdir, MAX_PATH - 1);
		free(subdir);
		read_scale_mode(wp.current_dir);
		load_random_image();
		fprintf(stderr, "wallpaper: switched to %s\n", wp.current_dir);
	}
}

void wallpaper_prev_dir(void) {
	/* With random selection, prev just picks another random dir */
	wallpaper_next_dir();
}

int wallpaper_timer_callback(void *data) {
	load_random_image();

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

	/* Reload current image at new size */
	if (wp.current_file[0] != '\0') {
		load_image_file(wp.current_file);
	} else {
		load_random_image();
	}
}

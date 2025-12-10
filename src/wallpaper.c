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

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef EXTRAS
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#endif

/* Fallback gradient colors - dark metallic pastel purples */
#define GRADIENT_COLOR1_R 0x2D
#define GRADIENT_COLOR1_G 0x1F
#define GRADIENT_COLOR1_B 0x3D
#define GRADIENT_COLOR2_R 0x4A
#define GRADIENT_COLOR2_G 0x3B
#define GRADIENT_COLOR2_B 0x5C
#define GRADIENT_ANGLE 33.0 /* degrees */

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

#ifdef EXTRAS
	/* Shader state */
	int is_shader;
	GLuint shader_program;
	GLuint vbo;
	GLuint fbo;
	GLuint render_texture;
	GLint u_time;
	GLint u_resolution;
	struct wl_event_source *shader_timer;
	float shader_time;
#endif
} wp;

/* Forward declarations */
static void load_random_image(void);
static void load_gradient_fallback(void);
static void read_scale_mode(const char *dir_path);
static char *expand_path(const char *path);
static char *pick_random_subdir(const char *path);
static char *pick_random_image(const char *dir_path);
#ifdef EXTRAS
static int is_shader_file(const char *name);
static char *pick_random_shader(const char *dir_path);
static int load_shader_file(const char *path);
static void render_shader_frame(void);
static void cleanup_shader(void);
static int shader_frame_callback(void *data);
#endif

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

#ifdef EXTRAS
static int is_shader_file(const char *name) {
	const char *ext = strrchr(name, '.');
	if (!ext)
		return 0;
	ext++;
	return (strcasecmp(ext, "glsl") == 0 ||
	        strcasecmp(ext, "frag") == 0);
}
#endif

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

/* Read .default file to get startup directory, returns allocated string or NULL */
static char *read_default_dir(const char *base_path) {
	char path[MAX_PATH];
	char buf[MAX_PATH];
	char full_path[MAX_PATH];
	FILE *f;
	size_t len;

	snprintf(path, sizeof(path), "%s/.default", base_path);
	f = fopen(path, "r");
	if (!f)
		return NULL;

	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return NULL;
	}

	fclose(f);

	/* Strip newline */
	len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	/* Build full path */
	snprintf(full_path, sizeof(full_path), "%s/%s", base_path, buf);

	/* Verify it's a directory */
	if (!is_directory(full_path)) {
		fprintf(stderr, "wallpaper: .default directory not found: %s\n", full_path);
		return NULL;
	}

	return strdup(full_path);
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

static void load_gradient_fallback(void) {
	unsigned char *data;
	size_t stride;
	double angle_rad, cos_a, sin_a;
	double max_proj;
	int x, y;
	WallpaperBuffer *buffer;

	if (wp.width == 0 || wp.height == 0)
		return;

	stride = wp.width * 4;
	data = calloc(1, stride * wp.height);
	if (!data)
		return;

	/* Convert angle to radians */
	angle_rad = GRADIENT_ANGLE * M_PI / 180.0;
	cos_a = cos(angle_rad);
	sin_a = sin(angle_rad);

	/* Calculate max projection for normalization */
	max_proj = fabs(wp.width * cos_a) + fabs(wp.height * sin_a);

	for (y = 0; y < wp.height; y++) {
		for (x = 0; x < wp.width; x++) {
			double proj, t;
			unsigned char r, g, b, *pixel;

			/* Project point onto gradient axis */
			proj = x * cos_a + y * sin_a;
			t = (proj + max_proj / 2.0) / max_proj;

			/* Clamp t to [0, 1] */
			if (t < 0.0) t = 0.0;
			if (t > 1.0) t = 1.0;

			/* Interpolate colors */
			r = (unsigned char)(GRADIENT_COLOR1_R + t * (GRADIENT_COLOR2_R - GRADIENT_COLOR1_R));
			g = (unsigned char)(GRADIENT_COLOR1_G + t * (GRADIENT_COLOR2_G - GRADIENT_COLOR1_G));
			b = (unsigned char)(GRADIENT_COLOR1_B + t * (GRADIENT_COLOR2_B - GRADIENT_COLOR1_B));

			/* BGRA format */
			pixel = data + (y * wp.width + x) * 4;
			pixel[0] = b;
			pixel[1] = g;
			pixel[2] = r;
			pixel[3] = 0xFF;
		}
	}

	/* Create new buffer */
	buffer = calloc(1, sizeof(WallpaperBuffer));
	if (!buffer) {
		free(data);
		return;
	}

	wlr_buffer_init(&buffer->base, &buffer_impl, wp.width, wp.height);
	buffer->data = data;
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

#ifdef EXTRAS
/* Default vertex shader for fullscreen quad */
static const char *default_vertex_shader =
	"#version 100\n"
	"attribute vec2 position;\n"
	"varying vec2 fragCoord;\n"
	"uniform vec2 resolution;\n"
	"void main() {\n"
	"    gl_Position = vec4(position, 0.0, 1.0);\n"
	"    fragCoord = (position * 0.5 + 0.5) * resolution;\n"
	"}\n";

/* Read shader source from file */
static char *read_shader_source(const char *path) {
	FILE *f;
	long len;
	char *source;

	f = fopen(path, "r");
	if (!f) return NULL;

	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);

	source = malloc(len + 1);
	if (!source) {
		fclose(f);
		return NULL;
	}

	if (fread(source, 1, len, f) != (size_t)len) {
		free(source);
		fclose(f);
		return NULL;
	}
	source[len] = '\0';
	fclose(f);
	return source;
}

/* Compile a shader and return its ID */
static GLuint compile_shader(GLenum type, const char *source) {
	GLuint shader;
	GLint status;
	GLchar log[512];

	shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		fprintf(stderr, "wallpaper: shader compile error: %s\n", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

/* Pick a random shader from directory */
static char *pick_random_shader(const char *dir_path) {
	DIR *dir;
	struct dirent *entry;
	char **shaders = NULL;
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

		if (is_shader_file(entry->d_name)) {
			snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
			if (count >= capacity) {
				capacity = capacity ? capacity * 2 : 16;
				shaders = realloc(shaders, capacity * sizeof(char *));
			}
			shaders[count++] = strdup(full_path);
		}
	}
	closedir(dir);

	if (count > 0) {
		int idx = rand() % count;
		result = shaders[idx];
		shaders[idx] = NULL;
	}

	for (int i = 0; i < count; i++) {
		if (shaders[i])
			free(shaders[i]);
	}
	free(shaders);

	return result;
}

/* Clean up shader resources */
static void cleanup_shader(void) {
	struct wlr_egl *egl;
	EGLDisplay display;
	EGLContext context;
	EGLContext prev_context;
	EGLSurface prev_draw, prev_read;
	int have_gl_resources;

	if (wp.shader_timer) {
		wl_event_source_remove(wp.shader_timer);
		wp.shader_timer = NULL;
	}

	/* Check if we have GL resources to clean up */
	have_gl_resources = wp.shader_program || wp.vbo || wp.fbo || wp.render_texture;

	if (have_gl_resources && wp.renderer && wlr_renderer_is_gles2(wp.renderer)) {
		egl = wlr_gles2_renderer_get_egl(wp.renderer);
		if (egl) {
			display = wlr_egl_get_display(egl);
			context = wlr_egl_get_context(egl);

			/* Save and set context */
			prev_context = eglGetCurrentContext();
			prev_draw = eglGetCurrentSurface(EGL_DRAW);
			prev_read = eglGetCurrentSurface(EGL_READ);

			if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
				if (wp.shader_program) {
					glDeleteProgram(wp.shader_program);
					wp.shader_program = 0;
				}
				if (wp.vbo) {
					glDeleteBuffers(1, &wp.vbo);
					wp.vbo = 0;
				}
				if (wp.fbo) {
					glDeleteFramebuffers(1, &wp.fbo);
					wp.fbo = 0;
				}
				if (wp.render_texture) {
					glDeleteTextures(1, &wp.render_texture);
					wp.render_texture = 0;
				}

				/* Restore context */
				eglMakeCurrent(display, prev_draw, prev_read, prev_context);
			}
		}
	}

	wp.is_shader = 0;
	wp.shader_time = 0.0f;
}

/* Load and compile a shader file */
static int load_shader_file(const char *path) {
	char *frag_source;
	GLuint vert_shader, frag_shader;
	GLint status;
	GLchar log[512];
	struct wlr_egl *egl;
	EGLDisplay display;
	EGLContext context;
	static const float vertices[] = {
		-1.0f, -1.0f,
		 1.0f, -1.0f,
		-1.0f,  1.0f,
		 1.0f,  1.0f,
	};

	/* Clean up any existing shader */
	cleanup_shader();

	/* Check if renderer is GLES2 */
	if (!wlr_renderer_is_gles2(wp.renderer)) {
		fprintf(stderr, "wallpaper: shaders require GLES2 renderer\n");
		return 0;
	}

	/* Get EGL context from wlroots */
	egl = wlr_gles2_renderer_get_egl(wp.renderer);
	if (!egl) {
		fprintf(stderr, "wallpaper: failed to get EGL from renderer\n");
		return 0;
	}

	display = wlr_egl_get_display(egl);
	context = wlr_egl_get_context(egl);

	/* Make EGL context current */
	if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
		fprintf(stderr, "wallpaper: failed to make EGL context current\n");
		return 0;
	}

	/* Read fragment shader source */
	frag_source = read_shader_source(path);
	if (!frag_source) {
		fprintf(stderr, "wallpaper: failed to read shader %s\n", path);
		return 0;
	}

	/* Compile vertex shader */
	vert_shader = compile_shader(GL_VERTEX_SHADER, default_vertex_shader);
	if (!vert_shader) {
		free(frag_source);
		return 0;
	}

	/* Compile fragment shader */
	frag_shader = compile_shader(GL_FRAGMENT_SHADER, frag_source);
	free(frag_source);
	if (!frag_shader) {
		glDeleteShader(vert_shader);
		return 0;
	}

	/* Link program */
	wp.shader_program = glCreateProgram();
	glAttachShader(wp.shader_program, vert_shader);
	glAttachShader(wp.shader_program, frag_shader);
	glLinkProgram(wp.shader_program);

	glDeleteShader(vert_shader);
	glDeleteShader(frag_shader);

	glGetProgramiv(wp.shader_program, GL_LINK_STATUS, &status);
	if (!status) {
		glGetProgramInfoLog(wp.shader_program, sizeof(log), NULL, log);
		fprintf(stderr, "wallpaper: shader link error: %s\n", log);
		glDeleteProgram(wp.shader_program);
		wp.shader_program = 0;
		return 0;
	}

	/* Get uniform locations */
	wp.u_time = glGetUniformLocation(wp.shader_program, "time");
	wp.u_resolution = glGetUniformLocation(wp.shader_program, "resolution");

	/* Create VBO for fullscreen quad */
	glGenBuffers(1, &wp.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, wp.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	/* Create FBO and texture for offscreen rendering */
	glGenFramebuffers(1, &wp.fbo);
	glGenTextures(1, &wp.render_texture);

	wp.is_shader = 1;
	wp.shader_time = 0.0f;
	strncpy(wp.current_file, path, MAX_PATH - 1);

	fprintf(stderr, "wallpaper: loaded shader %s\n", path);

	/* Animation disabled for now - render single frame on resize */

	return 1;
}

/* Render one frame of the shader */
static void render_shader_frame(void) {
	struct wlr_egl *egl;
	EGLDisplay display;
	EGLContext context;
	EGLContext prev_context;
	EGLSurface prev_draw, prev_read;
	WallpaperBuffer *buffer;
	unsigned char *data;
	size_t stride;
	GLint pos_attrib;

	if (!wp.is_shader || !wp.shader_program || wp.width == 0 || wp.height == 0)
		return;

	/* Get EGL context */
	egl = wlr_gles2_renderer_get_egl(wp.renderer);
	if (!egl) return;

	display = wlr_egl_get_display(egl);
	context = wlr_egl_get_context(egl);

	/* Save current EGL state */
	prev_context = eglGetCurrentContext();
	prev_draw = eglGetCurrentSurface(EGL_DRAW);
	prev_read = eglGetCurrentSurface(EGL_READ);

	if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
		return;

	/* Setup offscreen framebuffer */
	glBindFramebuffer(GL_FRAMEBUFFER, wp.fbo);

	glBindTexture(GL_TEXTURE_2D, wp.render_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, wp.width, wp.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, wp.render_texture, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "wallpaper: framebuffer incomplete\n");
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return;
	}

	glViewport(0, 0, wp.width, wp.height);

	/* Run shader */
	glUseProgram(wp.shader_program);

	if (wp.u_time >= 0)
		glUniform1f(wp.u_time, wp.shader_time);
	if (wp.u_resolution >= 0)
		glUniform2f(wp.u_resolution, (float)wp.width, (float)wp.height);

	glBindBuffer(GL_ARRAY_BUFFER, wp.vbo);
	pos_attrib = glGetAttribLocation(wp.shader_program, "position");
	glEnableVertexAttribArray(pos_attrib);
	glVertexAttribPointer(pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, 0);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(pos_attrib);

	/* Read pixels into CPU buffer */
	stride = wp.width * 4;
	data = malloc(stride * wp.height);
	if (!data) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return;
	}

	glReadPixels(0, 0, wp.width, wp.height, GL_RGBA, GL_UNSIGNED_BYTE, data);

	/* Convert RGBA to BGRA and flip vertically */
	buffer = calloc(1, sizeof(WallpaperBuffer));
	if (buffer) {
		int x, y;
		unsigned char *final_data = malloc(stride * wp.height);
		if (final_data) {
			for (y = 0; y < wp.height; y++) {
				int src_y = wp.height - 1 - y; /* Flip */
				for (x = 0; x < wp.width; x++) {
					unsigned char *src = data + (src_y * wp.width + x) * 4;
					unsigned char *dst = final_data + (y * wp.width + x) * 4;
					dst[0] = src[2]; /* B */
					dst[1] = src[1]; /* G */
					dst[2] = src[0]; /* R */
					dst[3] = src[3]; /* A */
				}
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
		} else {
			free(buffer);
		}
	}

	free(data);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	/* Restore previous EGL state */
	eglMakeCurrent(display, prev_draw, prev_read, prev_context);

	/* Increment time */
	wp.shader_time += 0.033f; /* ~30 FPS */
}

/* Timer callback for shader animation */
static int shader_frame_callback(void *data) {
	(void)data;

	if (wp.width > 0 && wp.height > 0) {
		render_shader_frame();
	}

	if (wp.shader_timer && wp.is_shader) {
		wl_event_source_timer_update(wp.shader_timer, 33); /* ~30 FPS */
	}

	return 0;
}
#endif /* EXTRAS */

static void load_random_image(void) {
	char *subdir, *image;
#ifdef EXTRAS
	char *shader;
#endif

	/* If we don't have a current directory, pick one */
	if (wp.current_dir[0] == '\0') {
		subdir = pick_random_subdir(wp.base_path);
		if (!subdir) {
			load_gradient_fallback();
			return;
		}
		strncpy(wp.current_dir, subdir, MAX_PATH - 1);
		free(subdir);
		read_scale_mode(wp.current_dir);
	}

#ifdef EXTRAS
	/* Try loading a shader first */
	shader = pick_random_shader(wp.current_dir);
	if (shader) {
		if (load_shader_file(shader)) {
			free(shader);
			return;
		}
		free(shader);
	}
	/* Clean up any shader state if we're loading an image */
	cleanup_shader();
#endif

	image = pick_random_image(wp.current_dir);
	if (!image) {
		/* No images in current dir, try picking a new subdir */
		subdir = pick_random_subdir(wp.base_path);
		if (subdir) {
			strncpy(wp.current_dir, subdir, MAX_PATH - 1);
			free(subdir);
			read_scale_mode(wp.current_dir);
#ifdef EXTRAS
			/* Try shaders in new dir */
			shader = pick_random_shader(wp.current_dir);
			if (shader) {
				if (load_shader_file(shader)) {
					free(shader);
					return;
				}
				free(shader);
			}
#endif
			image = pick_random_image(wp.current_dir);
		}
	}

	if (image) {
		load_image_file(image);
		free(image);
	} else {
		load_gradient_fallback();
	}
}

void wallpaper_init(struct wlr_scene *scene, struct wlr_renderer *renderer,
		const char *dir, int interval) {
	char *expanded;
	char *default_dir;

	srand(time(NULL));

	memset(&wp, 0, sizeof(wp));
	wp.scene = scene;
	wp.renderer = renderer;
	wp.interval = interval;
	wp.scale_mode = SCALE_COVER;

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

	/* Check for .default file to set startup directory */
	default_dir = read_default_dir(wp.base_path);
	if (default_dir) {
		strncpy(wp.current_dir, default_dir, MAX_PATH - 1);
		free(default_dir);
		read_scale_mode(wp.current_dir);
		fprintf(stderr, "wallpaper: using default directory %s\n", wp.current_dir);
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
#ifdef EXTRAS
	cleanup_shader();
#endif

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

void wallpaper_prev_image(void) {
	/* With random selection, prev just loads another random image */
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

#ifdef EXTRAS
	/* If shader is active, just render a new frame at new size */
	if (wp.is_shader) {
		render_shader_frame();
		return;
	}
#endif

	/* Reload current image at new size */
	if (wp.current_file[0] != '\0') {
		load_image_file(wp.current_file);
	} else {
		load_random_image();
	}
}

void wallpaper_disable(void) {
	if (wp.scene_buffer)
		wlr_scene_node_set_enabled(&wp.scene_buffer->node, false);
}

void wallpaper_enable(void) {
	if (wp.scene_buffer)
		wlr_scene_node_set_enabled(&wp.scene_buffer->node, true);
}

int wallpaper_is_enabled(void) {
	return wp.scene_buffer && wp.scene_buffer->node.enabled;
}

/* scripting.c - Wren scripting support for dwl */
#ifdef SCRIPTING

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wordexp.h>

#include "../wren/src/include/wren.h"
#include "scripting.h"

/*
 * Note: dwl functions are static, so we use script_* wrapper functions
 * defined in dwl.c that scripting.c can call.
 */
extern void script_spawn(const char *cmd);
extern void script_quit(void);
extern void script_focusstack(int dir);
extern void script_view(unsigned int tag);
extern void script_tag(unsigned int t);
extern void script_toggleview(unsigned int tag);
extern void script_toggletag(unsigned int tag);
extern void script_setmfact(float f);
extern void script_incnmaster(int n);
extern void script_killclient(void);
extern void script_togglefloating(void);
extern void script_togglefullscreen(void);
extern void script_focusmon(int dir);
extern void script_tagmon(int dir);

/* Wallpaper functions from wallpaper.c */
extern void wallpaper_disable(void);
extern void wallpaper_enable(void);
extern int wallpaper_is_enabled(void);
extern void wallpaper_next_image(void);
extern void wallpaper_prev_image(void);
extern void wallpaper_next_dir(void);
extern void wallpaper_prev_dir(void);

/* Global VM instance */
static WrenVM *vm = NULL;

/* Config path */
#define CONFIG_PATH "~/.config/dwl/init.wren"

/* Error handling */
static void wren_error(WrenVM *vm, WrenErrorType type, const char *module,
                       int line, const char *message) {
	switch (type) {
	case WREN_ERROR_COMPILE:
		fprintf(stderr, "[wren] Compile error in %s:%d: %s\n",
		        module ? module : "unknown", line, message);
		break;
	case WREN_ERROR_RUNTIME:
		fprintf(stderr, "[wren] Runtime error: %s\n", message);
		break;
	case WREN_ERROR_STACK_TRACE:
		fprintf(stderr, "[wren]   at %s:%d in %s\n",
		        module ? module : "unknown", line, message);
		break;
	}
}

static void wren_write(WrenVM *vm, const char *text) {
	fprintf(stderr, "%s", text);
}

/* ============================================================
 * Dwl class - core functions
 * ============================================================ */

static void dwl_spawn(WrenVM *vm) {
	const char *cmd = wrenGetSlotString(vm, 1);
	script_spawn(cmd);
}

static void dwl_quit(WrenVM *vm) {
	script_quit();
}

static void dwl_focusNext(WrenVM *vm) {
	script_focusstack(1);
}

static void dwl_focusPrev(WrenVM *vm) {
	script_focusstack(-1);
}

static void dwl_viewTag(WrenVM *vm) {
	unsigned int tag_num = (unsigned int)wrenGetSlotDouble(vm, 1);
	script_view(1u << tag_num);
}

static void dwl_viewAll(WrenVM *vm) {
	script_view(~0u);
}

static void dwl_tagClient(WrenVM *vm) {
	unsigned int tag_num = (unsigned int)wrenGetSlotDouble(vm, 1);
	script_tag(1u << tag_num);
}

static void dwl_toggleViewTag(WrenVM *vm) {
	unsigned int tag_num = (unsigned int)wrenGetSlotDouble(vm, 1);
	script_toggleview(1u << tag_num);
}

static void dwl_toggleTagClient(WrenVM *vm) {
	unsigned int tag_num = (unsigned int)wrenGetSlotDouble(vm, 1);
	script_toggletag(1u << tag_num);
}

static void dwl_killClient(WrenVM *vm) {
	script_killclient();
}

static void dwl_toggleFloating(WrenVM *vm) {
	script_togglefloating();
}

static void dwl_toggleFullscreen(WrenVM *vm) {
	script_togglefullscreen();
}

static void dwl_setMfact(WrenVM *vm) {
	float f = (float)wrenGetSlotDouble(vm, 1);
	script_setmfact(f);
}

static void dwl_incNmaster(WrenVM *vm) {
	int i = (int)wrenGetSlotDouble(vm, 1);
	script_incnmaster(i);
}

static void dwl_focusMonitor(WrenVM *vm) {
	int dir = (int)wrenGetSlotDouble(vm, 1);
	script_focusmon(dir);
}

static void dwl_tagMonitor(WrenVM *vm) {
	int dir = (int)wrenGetSlotDouble(vm, 1);
	script_tagmon(dir);
}

static void dwl_log(WrenVM *vm) {
	const char *msg = wrenGetSlotString(vm, 1);
	fprintf(stderr, "[wren] %s\n", msg);

	/* Also write to log file */
	FILE *f = fopen("/tmp/dwl-wren.log", "a");
	if (f) {
		fprintf(f, "%s\n", msg);
		fclose(f);
	}
}

/* ============================================================
 * Wallpaper class - wallpaper control
 * ============================================================ */

static void wp_disable(WrenVM *vm) {
	wallpaper_disable();
}

static void wp_enable(WrenVM *vm) {
	wallpaper_enable();
}

static void wp_isEnabled(WrenVM *vm) {
	wrenSetSlotBool(vm, 0, wallpaper_is_enabled());
}

static void wp_nextImage(WrenVM *vm) {
	wallpaper_next_image();
}

static void wp_prevImage(WrenVM *vm) {
	wallpaper_prev_image();
}

static void wp_nextDir(WrenVM *vm) {
	wallpaper_next_dir();
}

static void wp_prevDir(WrenVM *vm) {
	wallpaper_prev_dir();
}

/* ============================================================
 * Hooks class - event callbacks
 * ============================================================ */

/* Store hook handles */
static WrenHandle *hook_handles[16] = {0};
static const char *hook_names[] = {
	"startup", "quit", "clientCreate", "clientDestroy",
	"clientFocus", "tagChange", "layoutChange",
	"monitorConnect", "monitorDisconnect", NULL
};

enum {
	HOOK_STARTUP = 0,
	HOOK_QUIT,
	HOOK_CLIENT_CREATE,
	HOOK_CLIENT_DESTROY,
	HOOK_CLIENT_FOCUS,
	HOOK_TAG_CHANGE,
	HOOK_LAYOUT_CHANGE,
	HOOK_MONITOR_CONNECT,
	HOOK_MONITOR_DISCONNECT,
	HOOK_COUNT
};

static void hooks_on(WrenVM *vm) {
	const char *event = wrenGetSlotString(vm, 1);

	for (int i = 0; hook_names[i]; i++) {
		if (strcmp(event, hook_names[i]) == 0) {
			if (hook_handles[i])
				wrenReleaseHandle(vm, hook_handles[i]);
			hook_handles[i] = wrenGetSlotHandle(vm, 2);
			return;
		}
	}
	fprintf(stderr, "[wren] Unknown hook: %s\n", event);
}

static void call_hook(int hook_id) {
	if (!vm || !hook_handles[hook_id])
		return;

	wrenEnsureSlots(vm, 1);
	wrenSetSlotHandle(vm, 0, hook_handles[hook_id]);
	wrenCall(vm, wrenMakeCallHandle(vm, "call()"));
}

/* ============================================================
 * Keys class - runtime keybinds
 * ============================================================ */

/* Key binding storage */
#define MAX_SCRIPT_KEYS 64

typedef struct {
	unsigned int mod;
	unsigned int key;
	WrenHandle *callback;
} ScriptKey;

static ScriptKey script_keys[MAX_SCRIPT_KEYS];
static int script_key_count = 0;

static unsigned int parse_mod(const char *mod_str) {
	unsigned int mod = 0;
	if (strstr(mod_str, "mod") || strstr(mod_str, "super") || strstr(mod_str, "logo"))
		mod |= (1 << 6); /* WLR_MODIFIER_LOGO */
	if (strstr(mod_str, "shift"))
		mod |= (1 << 0); /* WLR_MODIFIER_SHIFT */
	if (strstr(mod_str, "ctrl") || strstr(mod_str, "control"))
		mod |= (1 << 2); /* WLR_MODIFIER_CTRL */
	if (strstr(mod_str, "alt"))
		mod |= (1 << 3); /* WLR_MODIFIER_ALT */
	return mod;
}

static void keys_bind(WrenVM *vm) {
	if (script_key_count >= MAX_SCRIPT_KEYS) {
		fprintf(stderr, "[wren] Max keybinds reached\n");
		return;
	}

	const char *mod_str = wrenGetSlotString(vm, 1);
	const char *key_str = wrenGetSlotString(vm, 2);

	ScriptKey *k = &script_keys[script_key_count++];
	k->mod = parse_mod(mod_str);
	k->key = key_str[0]; /* Simple: just use first char as keysym for now */
	k->callback = wrenGetSlotHandle(vm, 3);

	fprintf(stderr, "[wren] Bound key: %s+%s\n", mod_str, key_str);
}

/* Called from dwl's key handler to check script bindings */
bool scripting_handle_key(unsigned int mod, unsigned int key) {
	for (int i = 0; i < script_key_count; i++) {
		if (script_keys[i].mod == mod && script_keys[i].key == key) {
			wrenEnsureSlots(vm, 1);
			wrenSetSlotHandle(vm, 0, script_keys[i].callback);
			wrenCall(vm, wrenMakeCallHandle(vm, "call()"));
			return true;
		}
	}
	return false;
}

/* ============================================================
 * Foreign method binding
 * ============================================================ */

static WrenForeignMethodFn bind_foreign_method(WrenVM *vm, const char *module,
                                                const char *className,
                                                bool isStatic,
                                                const char *signature) {
	if (strcmp(module, "main") != 0)
		return NULL;

	if (strcmp(className, "Dwl") == 0) {
		if (strcmp(signature, "spawn(_)") == 0) return dwl_spawn;
		if (strcmp(signature, "quit()") == 0) return dwl_quit;
		if (strcmp(signature, "focusNext()") == 0) return dwl_focusNext;
		if (strcmp(signature, "focusPrev()") == 0) return dwl_focusPrev;
		if (strcmp(signature, "viewTag(_)") == 0) return dwl_viewTag;
		if (strcmp(signature, "viewAll()") == 0) return dwl_viewAll;
		if (strcmp(signature, "tagClient(_)") == 0) return dwl_tagClient;
		if (strcmp(signature, "toggleViewTag(_)") == 0) return dwl_toggleViewTag;
		if (strcmp(signature, "toggleTagClient(_)") == 0) return dwl_toggleTagClient;
		if (strcmp(signature, "killClient()") == 0) return dwl_killClient;
		if (strcmp(signature, "toggleFloating()") == 0) return dwl_toggleFloating;
		if (strcmp(signature, "toggleFullscreen()") == 0) return dwl_toggleFullscreen;
		if (strcmp(signature, "setMfact(_)") == 0) return dwl_setMfact;
		if (strcmp(signature, "incNmaster(_)") == 0) return dwl_incNmaster;
		if (strcmp(signature, "focusMonitor(_)") == 0) return dwl_focusMonitor;
		if (strcmp(signature, "tagMonitor(_)") == 0) return dwl_tagMonitor;
		if (strcmp(signature, "log(_)") == 0) return dwl_log;
	}

	if (strcmp(className, "Hooks") == 0) {
		if (strcmp(signature, "on(_,_)") == 0) return hooks_on;
	}

	if (strcmp(className, "Keys") == 0) {
		if (strcmp(signature, "bind(_,_,_)") == 0) return keys_bind;
	}

	if (strcmp(className, "Wallpaper") == 0) {
		if (strcmp(signature, "disable()") == 0) return wp_disable;
		if (strcmp(signature, "enable()") == 0) return wp_enable;
		if (strcmp(signature, "isEnabled") == 0) return wp_isEnabled;
		if (strcmp(signature, "nextImage()") == 0) return wp_nextImage;
		if (strcmp(signature, "prevImage()") == 0) return wp_prevImage;
		if (strcmp(signature, "nextDir()") == 0) return wp_nextDir;
		if (strcmp(signature, "prevDir()") == 0) return wp_prevDir;
	}

	return NULL;
}

/* ============================================================
 * Module loading
 * ============================================================ */

/* Prelude: class definitions for foreign methods */
static const char *prelude =
	"class Dwl {\n"
	"  foreign static spawn(cmd)\n"
	"  foreign static quit()\n"
	"  foreign static focusNext()\n"
	"  foreign static focusPrev()\n"
	"  foreign static viewTag(n)\n"
	"  foreign static viewAll()\n"
	"  foreign static tagClient(n)\n"
	"  foreign static toggleViewTag(n)\n"
	"  foreign static toggleTagClient(n)\n"
	"  foreign static killClient()\n"
	"  foreign static toggleFloating()\n"
	"  foreign static toggleFullscreen()\n"
	"  foreign static setMfact(f)\n"
	"  foreign static incNmaster(n)\n"
	"  foreign static focusMonitor(dir)\n"
	"  foreign static tagMonitor(dir)\n"
	"  foreign static log(msg)\n"
	"}\n"
	"\n"
	"class Hooks {\n"
	"  foreign static on(event, fn)\n"
	"}\n"
	"\n"
	"class Keys {\n"
	"  foreign static bind(mod, key, fn)\n"
	"}\n"
	"\n"
	"class Wallpaper {\n"
	"  foreign static disable()\n"
	"  foreign static enable()\n"
	"  foreign static isEnabled\n"
	"  foreign static nextImage()\n"
	"  foreign static prevImage()\n"
	"  foreign static nextDir()\n"
	"  foreign static prevDir()\n"
	"}\n";

static char *expand_path(const char *path) {
	wordexp_t exp;
	char *result = NULL;

	if (wordexp(path, &exp, WRDE_NOCMD) == 0) {
		if (exp.we_wordc > 0)
			result = strdup(exp.we_wordv[0]);
		wordfree(&exp);
	}
	return result;
}

static char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	char *buf = malloc(size + 1);
	if (buf) {
		fread(buf, 1, size, f);
		buf[size] = '\0';
	}
	fclose(f);
	return buf;
}

/* ============================================================
 * Public API
 * ============================================================ */

bool scripting_init(void) {
	WrenConfiguration config;
	wrenInitConfiguration(&config);
	config.errorFn = wren_error;
	config.writeFn = wren_write;
	config.bindForeignMethodFn = bind_foreign_method;

	vm = wrenNewVM(&config);
	if (!vm) {
		fprintf(stderr, "[wren] Failed to create VM\n");
		return false;
	}

	/* Load prelude with class definitions */
	WrenInterpretResult result = wrenInterpret(vm, "main", prelude);
	if (result != WREN_RESULT_SUCCESS) {
		fprintf(stderr, "[wren] Failed to load prelude\n");
		scripting_cleanup();
		return false;
	}

	/* Load user init script if it exists */
	char *config_path = expand_path(CONFIG_PATH);
	if (config_path) {
		char *source = read_file(config_path);
		if (source) {
			fprintf(stderr, "[wren] Loading %s\n", config_path);
			result = wrenInterpret(vm, "main", source);
			if (result != WREN_RESULT_SUCCESS)
				fprintf(stderr, "[wren] Failed to load init script\n");
			free(source);
		}
		free(config_path);
	}

	return true;
}

void scripting_cleanup(void) {
	if (!vm) return;

	/* Release hook handles */
	for (int i = 0; i < HOOK_COUNT; i++) {
		if (hook_handles[i]) {
			wrenReleaseHandle(vm, hook_handles[i]);
			hook_handles[i] = NULL;
		}
	}

	/* Release key callback handles */
	for (int i = 0; i < script_key_count; i++) {
		if (script_keys[i].callback)
			wrenReleaseHandle(vm, script_keys[i].callback);
	}
	script_key_count = 0;

	wrenFreeVM(vm);
	vm = NULL;
}

void scripting_hook(const char *hook_name) {
	for (int i = 0; hook_names[i]; i++) {
		if (strcmp(hook_name, hook_names[i]) == 0) {
			call_hook(i);
			return;
		}
	}
}

void scripting_on_startup(void) { call_hook(HOOK_STARTUP); }
void scripting_on_quit(void) { call_hook(HOOK_QUIT); }
void scripting_on_client_create(void *client) { call_hook(HOOK_CLIENT_CREATE); }
void scripting_on_client_destroy(void *client) { call_hook(HOOK_CLIENT_DESTROY); }
void scripting_on_client_focus(void *client) { call_hook(HOOK_CLIENT_FOCUS); }
void scripting_on_tag_change(unsigned int tags) { call_hook(HOOK_TAG_CHANGE); }
void scripting_on_layout_change(void *monitor) { call_hook(HOOK_LAYOUT_CHANGE); }
void scripting_on_monitor_connect(void *monitor) { call_hook(HOOK_MONITOR_CONNECT); }
void scripting_on_monitor_disconnect(void *monitor) { call_hook(HOOK_MONITOR_DISCONNECT); }

bool scripting_eval(const char *source) {
	if (!vm) return false;
	return wrenInterpret(vm, "main", source) == WREN_RESULT_SUCCESS;
}

bool scripting_run_file(const char *path) {
	char *source = read_file(path);
	if (!source) return false;
	bool result = scripting_eval(source);
	free(source);
	return result;
}

void scripting_reload(void) {
	fprintf(stderr, "[wren] Reloading scripts...\n");
	scripting_cleanup();
	scripting_init();
	scripting_on_startup();
}

#endif /* SCRIPTING */

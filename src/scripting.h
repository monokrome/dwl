/* scripting.h - Wren scripting support for dwl */
#ifndef SCRIPTING_H
#define SCRIPTING_H

#ifdef SCRIPTING

#include <stdbool.h>

/* Initialize the Wren VM and load init script */
bool scripting_init(void);

/* Clean up the Wren VM */
void scripting_cleanup(void);

/* Call a hook by name with optional arguments */
void scripting_hook(const char *hook_name);

/* Hooks called from dwl */
void scripting_on_startup(void);
void scripting_on_quit(void);
void scripting_on_client_create(void *client);
void scripting_on_client_destroy(void *client);
void scripting_on_client_focus(void *client);
void scripting_on_tag_change(unsigned int tags);
void scripting_on_layout_change(void *monitor);
void scripting_on_monitor_connect(void *monitor);
void scripting_on_monitor_disconnect(void *monitor);

/* Execute a wren script string */
bool scripting_eval(const char *source);

/* Execute a wren script file */
bool scripting_run_file(const char *path);

/* Reload the scripting system (cleanup + init) */
void scripting_reload(void);

/* Handle key press - returns true if handled by script */
bool scripting_handle_key(unsigned int mod, unsigned int key);

#else /* SCRIPTING */

/* No-op stubs when scripting is disabled */
#define scripting_init() (true)
#define scripting_cleanup() ((void)0)
#define scripting_hook(name) ((void)0)
#define scripting_on_startup() ((void)0)
#define scripting_on_quit() ((void)0)
#define scripting_on_client_create(c) ((void)0)
#define scripting_on_client_destroy(c) ((void)0)
#define scripting_on_client_focus(c) ((void)0)
#define scripting_on_tag_change(t) ((void)0)
#define scripting_on_layout_change(m) ((void)0)
#define scripting_on_monitor_connect(m) ((void)0)
#define scripting_on_monitor_disconnect(m) ((void)0)
#define scripting_eval(s) (true)
#define scripting_run_file(p) (true)
#define scripting_reload() ((void)0)
#define scripting_handle_key(m, k) (false)

#endif /* SCRIPTING */

#endif /* SCRIPTING_H */

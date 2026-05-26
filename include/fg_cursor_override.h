/*
 * fg_cursor_override.h — Wine cursor receiver.
 *
 * Implements yetios_cursor_manager_v1 server-side.
 * Wine sends a surface with the Win32 arrow at boot.
 * We copy the pixels into a compositor-owned wlr_buffer and store it
 * permanently.  That buffer is THE cursor.  Forever.
 */

#ifndef FG_CURSOR_OVERRIDE_H
#define FG_CURSOR_OVERRIDE_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct fg_server;

struct fg_cursor_override {
    struct fg_server *server;
    struct wl_global *global;
};

/* Create the wl_global.  Call after wl_display_create(). */
struct fg_cursor_override *cursor_override_init(struct fg_server *server);

/* Tear down.  Safe with NULL. */
void cursor_override_destroy(struct fg_cursor_override *co);

#endif /* FG_CURSOR_OVERRIDE_H */
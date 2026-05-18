/*
 * fg_input.h — Keyboard, cursor, and seat management for frostedglass.
 */

#ifndef FG_INPUT_H
#define FG_INPUT_H

#include "fg_types.h"

/*
 * Listener callback for backend new_input events.
 * Routes to keyboard or pointer setup and updates seat capabilities.
 */
void server_new_input(struct wl_listener *listener, void *data);

/*
 * Cursor event listener callbacks — wired to server->cursor_motion, etc.
 */
void cursor_motion(struct wl_listener *listener, void *data);
void cursor_motion_absolute(struct wl_listener *listener, void *data);
void cursor_button(struct wl_listener *listener, void *data);
void cursor_axis(struct wl_listener *listener, void *data);
void cursor_frame(struct wl_listener *listener, void *data);

/*
 * Seat request listener callbacks.
 */
void seat_request_cursor(struct wl_listener *listener, void *data);
void seat_request_set_selection(struct wl_listener *listener, void *data);

#endif /* FG_INPUT_H */

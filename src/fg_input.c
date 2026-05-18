/*
 * fg_input.c — Keyboard, cursor, and seat management for frostedglass.
 *
 * Owns keyboard creation/keymap setup, compositor keybindings
 * (Ctrl+Alt+Backspace = kill), pointer motion/button/axis dispatch,
 * and seat capability negotiation.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>

#include <xkbcommon/xkbcommon.h>

#include "fg_input.h"
#include "fg_toplevel.h"   /* focus_toplevel(), toplevel_at() */

/* ------------------------------------------------------------------ */
/* Compositor keybindings                                             */
/* ------------------------------------------------------------------ */

static bool handle_compositor_keybinding(struct fg_server *server,
    xkb_keysym_t sym, uint32_t modifiers) {
    /*
     * Minimal compositor bindings — we intentionally keep very few because
     * Wine handles Alt+F4, Alt+Tab, etc. through its own Win32 logic.
     *
     * Ctrl+Alt+Backspace = emergency kill compositor (escape hatch).
     * Ctrl+Alt+Delete    = pass through to Wine (it sees it as CAD).
     */
    if ((modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) ==
        (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) {
        if (sym == XKB_KEY_BackSpace) {
            wl_display_terminate(server->display);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Keyboard listeners                                                 */
/* ------------------------------------------------------------------ */

static void keyboard_key(struct wl_listener *listener, void *data) {
    struct fg_keyboard *keyboard =
        wl_container_of(listener, keyboard, key);
    struct fg_server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server->seat;

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(
        keyboard->wlr_keyboard->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers =
        wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled = handle_compositor_keybinding(server, syms[i],
                modifiers);
            if (handled) break;
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec,
            event->keycode, event->state);
    }
}

static void keyboard_modifiers(struct wl_listener *listener, void *data) {
    struct fg_keyboard *keyboard =
        wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat,
        keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
        &keyboard->wlr_keyboard->modifiers);
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
    struct fg_keyboard *keyboard =
        wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void server_new_keyboard(struct fg_server *server,
    struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard =
        wlr_keyboard_from_input_device(device);

    struct fg_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    keyboard->modifiers.notify = keyboard_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);

    keyboard->key.notify = keyboard_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);

    keyboard->destroy.notify = keyboard_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, wlr_keyboard);
    wl_list_insert(&server->keyboards, &keyboard->link);
}

/* ------------------------------------------------------------------ */
/* Cursor / pointer motion                                            */
/* ------------------------------------------------------------------ */

static void process_cursor_motion(struct fg_server *server,
    uint32_t time) {
    if (server->cursor_mode == FG_CURSOR_MOVE) {
        struct fg_toplevel *tl = server->grabbed_toplevel;
        wlr_scene_node_set_position(&tl->scene_tree->node,
            server->cursor->x - server->grab_x,
            server->cursor->y - server->grab_y);
        return;
    } else if (server->cursor_mode == FG_CURSOR_RESIZE) {
        struct fg_toplevel *tl = server->grabbed_toplevel;
        double bx = server->cursor->x - server->grab_x;
        double by = server->cursor->y - server->grab_y;
        int new_left   = server->grab_geobox.x;
        int new_right  = server->grab_geobox.x + server->grab_geobox.width;
        int new_top    = server->grab_geobox.y;
        int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

        if (server->resize_edges & WLR_EDGE_TOP) {
            new_top = by;
            if (new_top >= new_bottom) new_top = new_bottom - 1;
        } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
            new_bottom = by;
            if (new_bottom <= new_top) new_bottom = new_top + 1;
        }
        if (server->resize_edges & WLR_EDGE_LEFT) {
            new_left = bx;
            if (new_left >= new_right) new_left = new_right - 1;
        } else if (server->resize_edges & WLR_EDGE_RIGHT) {
            new_right = bx;
            if (new_right <= new_left) new_right = new_left + 1;
        }

        struct wlr_box geo;
        wlr_xdg_surface_get_geometry(tl->xdg_toplevel->base, &geo);
        wlr_scene_node_set_position(&tl->scene_tree->node,
            new_left - geo.x, new_top - geo.y);
        wlr_xdg_toplevel_set_size(tl->xdg_toplevel,
            new_right - new_left, new_bottom - new_top);
        return;
    }

    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct fg_toplevel *toplevel =
        toplevel_at(server, server->cursor->x, server->cursor->y,
            &surface, &sx, &sy);

    if (!toplevel) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
            "default");
    }

    if (surface) {
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(server->seat);
    }
}

void cursor_motion(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base,
        event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

void cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
        event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

void cursor_button(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
        event->button, event->state);

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (server->cursor_mode != FG_CURSOR_PASSTHROUGH) {
            server->cursor_mode = FG_CURSOR_PASSTHROUGH;
            server->grabbed_toplevel = NULL;
        }
    } else {
        double sx, sy;
        struct wlr_surface *surface;
        struct fg_toplevel *toplevel = toplevel_at(server,
            server->cursor->x, server->cursor->y, &surface, &sx, &sy);
        focus_toplevel(toplevel);
    }
}

void cursor_axis(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
        event->orientation, event->delta, event->delta_discrete,
        event->source, event->relative_direction);
}

void cursor_frame(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

/* ------------------------------------------------------------------ */
/* Seat requests                                                      */
/* ------------------------------------------------------------------ */

void seat_request_cursor(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, request_set_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused =
        server->seat->pointer_state.focused_client;
    if (focused == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface,
            event->hotspot_x, event->hotspot_y);
    }
}

void seat_request_set_selection(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* ------------------------------------------------------------------ */
/* Input device routing                                               */
/* ------------------------------------------------------------------ */

static void server_new_pointer(struct fg_server *server,
    struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}

void server_new_input(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        server_new_pointer(server, device);
        break;
    default:
        break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

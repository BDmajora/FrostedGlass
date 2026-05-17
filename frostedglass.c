/*
 * frostedglass.c — minimal wlroots compositor for YetiOS.
 *
 * Design goals:
 *   - Zero unnecessary latency. No animations, no blur, no decorations.
 *   - Wine IS the desktop. explorer.exe draws the taskbar, manages windows.
 *   - The compositor is invisible plumbing: surfaces in, scanout out.
 *   - Integrates with snowfall's DRM master handoff (snowfall exits when
 *     another process acquires DRM master; wlroots does this at startup).
 *
 * Architecture:
 *   wlroots backend (DRM/KMS + libinput) → wl_compositor + xdg_shell
 *   → Wine Wayland driver creates xdg_toplevels for each Win32 window
 *   → wlroots renderer composites (or direct-scanout for single surface)
 *
 * Build:
 *   meson setup build && ninja -C build
 *
 * Runtime:
 *   Launched as a Wayland session from snowfall (or from TTY for testing).
 *   Automatically spawns Wine explorer.exe with native Wayland driver.
 *
 * Part of the YetiOS snow suite:
 *   snowcone (boot splash) → snowfall (login) → frostedglass (compositor)
 *
 * Based on wlroots tinywl patterns, stripped to essentials + Wine launch.
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/util/log.h>

#include <xkbcommon/xkbcommon.h>

/* --------------------------------------------------------------------------
 * Structures
 * -------------------------------------------------------------------------- */

enum fg_cursor_mode {
    FG_CURSOR_PASSTHROUGH,
    FG_CURSOR_MOVE,
    FG_CURSOR_RESIZE,
};

struct fg_server {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;

    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;                 /* fg_output.link */
    struct wl_listener new_output;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_list toplevels;               /* fg_toplevel.link */
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;

    struct wlr_seat *seat;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    struct wl_list keyboards;               /* fg_keyboard.link */
    struct wl_listener new_input;
    struct wl_listener request_set_cursor;
    struct wl_listener request_set_selection;

    enum fg_cursor_mode cursor_mode;
    struct fg_toplevel *grabbed_toplevel;
    double grab_x, grab_y;
    struct wlr_box grab_geobox;
    uint32_t resize_edges;

    pid_t wine_pid;
    const char *wine_desktop_res;           /* e.g. "1920x1080" or NULL=auto */
};

struct fg_output {
    struct wl_list link;
    struct fg_server *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
};

struct fg_toplevel {
    struct wl_list link;
    struct fg_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
};

struct fg_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

struct fg_keyboard {
    struct wl_list link;
    struct fg_server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

/* --------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */

static void focus_toplevel(struct fg_toplevel *toplevel);
static struct fg_toplevel *toplevel_at(struct fg_server *server,
    double lx, double ly, struct wlr_surface **surface,
    double *sx, double *sy);

/* --------------------------------------------------------------------------
 * Output handling
 * -------------------------------------------------------------------------- */

static void output_frame(struct wl_listener *listener, void *data) {
    struct fg_output *output = wl_container_of(listener, output, frame);
    struct wlr_scene *scene = output->server->scene;

    struct wlr_scene_output *scene_output =
        wlr_scene_get_scene_output(scene, output->wlr_output);
    wlr_scene_output_commit(scene_output, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
    struct fg_output *output =
        wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    struct fg_output *output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    }
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    struct fg_output *output = calloc(1, sizeof(*output));
    output->wlr_output = wlr_output;
    output->server = server;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    struct wlr_output_layout_output *lo =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    output->scene_output =
        wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, lo,
        output->scene_output);
}

/* --------------------------------------------------------------------------
 * XDG toplevel handling — Wine creates one per Win32 window
 * -------------------------------------------------------------------------- */

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, map);
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
    focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, unmap);
    if (toplevel == toplevel->server->grabbed_toplevel) {
        toplevel->server->cursor_mode = FG_CURSOR_PASSTHROUGH;
        toplevel->server->grabbed_toplevel = NULL;
    }
    wl_list_remove(&toplevel->link);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, commit);
    if (toplevel->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
    }
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, destroy);
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    free(toplevel);
}

static void begin_interactive(struct fg_toplevel *toplevel,
    enum fg_cursor_mode mode, uint32_t edges) {
    struct fg_server *server = toplevel->server;
    server->grabbed_toplevel = toplevel;
    server->cursor_mode = mode;

    if (mode == FG_CURSOR_MOVE) {
        server->grab_x = server->cursor->x -
            toplevel->scene_tree->node.x;
        server->grab_y = server->cursor->y -
            toplevel->scene_tree->node.y;
    } else {
        struct wlr_box geo;
        wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
        double bx = (toplevel->scene_tree->node.x + geo.x) +
            ((edges & WLR_EDGE_RIGHT)  ? geo.width  : 0);
        double by = (toplevel->scene_tree->node.y + geo.y) +
            ((edges & WLR_EDGE_BOTTOM) ? geo.height : 0);
        server->grab_x = server->cursor->x - bx;
        server->grab_y = server->cursor->y - by;
        server->grab_geobox = geo;
        server->grab_geobox.x += toplevel->scene_tree->node.x;
        server->grab_geobox.y += toplevel->scene_tree->node.y;
        server->resize_edges = edges;
    }
}

static void xdg_toplevel_request_move(struct wl_listener *listener,
    void *data) {
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, FG_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener,
    void *data) {
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_resize);
    begin_interactive(toplevel, FG_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener,
    void *data) {
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_maximize);
    if (toplevel->xdg_toplevel->base->surface->mapped) {
        wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel,
            toplevel->xdg_toplevel->requested.maximized);
    }
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener,
    void *data) {
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_fullscreen);
    if (toplevel->xdg_toplevel->base->surface->mapped) {
        wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel,
            toplevel->xdg_toplevel->requested.fullscreen);
    }
}

static void server_new_xdg_toplevel(struct wl_listener *listener,
    void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct fg_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->scene_tree =
        wlr_scene_xdg_surface_create(&server->scene->tree,
            xdg_toplevel->base);
    toplevel->scene_tree->node.data = toplevel;
    xdg_toplevel->base->data = toplevel->scene_tree;

    toplevel->map.notify = xdg_toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);

    toplevel->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap,
        &toplevel->unmap);

    toplevel->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit,
        &toplevel->commit);

    toplevel->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

    toplevel->request_move.notify = xdg_toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move,
        &toplevel->request_move);

    toplevel->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize,
        &toplevel->request_resize);

    toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize,
        &toplevel->request_maximize);

    toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen,
        &toplevel->request_fullscreen);
}

/* --------------------------------------------------------------------------
 * XDG popup handling — Wine menus, tooltips, dropdowns
 * -------------------------------------------------------------------------- */

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    struct fg_popup *popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    struct fg_popup *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
    struct wlr_xdg_popup *xdg_popup = data;

    struct fg_popup *popup = calloc(1, sizeof(*popup));
    popup->xdg_popup = xdg_popup;

    struct wlr_xdg_surface *parent =
        wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    assert(parent);
    struct wlr_scene_tree *parent_tree = parent->data;
    xdg_popup->base->data =
        wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* --------------------------------------------------------------------------
 * Focus
 * -------------------------------------------------------------------------- */

static void focus_toplevel(struct fg_toplevel *toplevel) {
    if (!toplevel) return;
    struct fg_server *server = toplevel->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface *surface =
        toplevel->xdg_toplevel->base->surface;

    if (prev_surface == surface) return;

    if (prev_surface) {
        struct wlr_xdg_toplevel *prev_toplevel =
            wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_toplevel) {
            wlr_xdg_toplevel_set_activated(prev_toplevel, false);
        }
    }

    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    wl_list_remove(&toplevel->link);
    wl_list_insert(&server->toplevels, &toplevel->link);

    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

    struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
    if (kb) {
        wlr_seat_keyboard_notify_enter(seat, surface,
            kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }
}

static struct fg_toplevel *toplevel_at(struct fg_server *server,
    double lx, double ly, struct wlr_surface **surface,
    double *sx, double *sy) {
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;

    struct wlr_scene_buffer *scene_buffer =
        wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) return NULL;

    *surface = scene_surface->surface;

    struct wlr_scene_tree *tree = node->parent;
    while (tree && !tree->node.data) {
        tree = tree->node.parent;
    }
    return tree ? tree->node.data : NULL;
}

/* --------------------------------------------------------------------------
 * Keyboard
 * -------------------------------------------------------------------------- */

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

/* --------------------------------------------------------------------------
 * Cursor / pointer
 * -------------------------------------------------------------------------- */

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

static void cursor_motion(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base,
        event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener,
    void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
        event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static void cursor_button(struct wl_listener *listener, void *data) {
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

static void cursor_axis(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
        event->orientation, event->delta, event->delta_discrete,
        event->source, event->relative_direction);
}

static void cursor_frame(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
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

static void seat_request_set_selection(struct wl_listener *listener,
    void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* --------------------------------------------------------------------------
 * Input routing
 * -------------------------------------------------------------------------- */

static void server_new_pointer(struct fg_server *server,
    struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
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

/* --------------------------------------------------------------------------
 * Wine launcher — spawns explorer.exe as the desktop shell
 * -------------------------------------------------------------------------- */

static void launch_wine(struct fg_server *server, const char *socket) {
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log(WLR_ERROR, "fork() failed for Wine");
        return;
    }
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", socket, 1);
        unsetenv("DISPLAY");
        setenv("WINEWAYLAND", "1", 1);

        const char *res = server->wine_desktop_res;

        /* Apply registry prefs first (colors, shell, DPI). */
        const char *home = getenv("HOME");
        char reg_path[512];
        snprintf(reg_path, sizeof(reg_path),
            "%s/.frostedglass_prefs.reg", home);
        /* Fall back to legacy name */
        if (access(reg_path, R_OK) != 0) {
            snprintf(reg_path, sizeof(reg_path),
                "%s/.wslk_prefs.reg", home);
        }
        if (access(reg_path, R_OK) == 0) {
            pid_t reg_pid = fork();
            if (reg_pid == 0) {
                execlp("wine", "wine", "regedit", "/s", reg_path, NULL);
                _exit(127);
            }
            if (reg_pid > 0) waitpid(reg_pid, NULL, 0);
        }

        if (res) {
            char desktop_arg[128];
            snprintf(desktop_arg, sizeof(desktop_arg),
                "/desktop=shell,%s", res);
            execlp("wine", "wine", "explorer", desktop_arg, NULL);
        } else {
            execlp("wine", "wine", "explorer", "/desktop=shell", NULL);
        }
        _exit(127);
    }
    server->wine_pid = pid;
    wlr_log(WLR_INFO, "Wine explorer.exe launched (pid %d)", pid);
}

/* --------------------------------------------------------------------------
 * SIGCHLD handler — reap Wine if it crashes so we can exit cleanly
 * -------------------------------------------------------------------------- */

static struct fg_server *g_server = NULL;

static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (g_server && pid == g_server->wine_pid) {
            wlr_log(WLR_INFO, "Wine exited (status %d), shutting down",
                WEXITSTATUS(status));
            if (g_server->display) {
                wl_display_terminate(g_server->display);
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    wlr_log_init(WLR_DEBUG, NULL);

    const char *resolution = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--res") == 0 && i + 1 < argc) {
            resolution = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage: frostedglass [--res WxH]\n"
                "\n"
                "Minimal Wayland compositor for YetiOS.\n"
                "Launches Wine explorer.exe as the desktop shell.\n"
                "\n"
                "Part of the snow suite:\n"
                "  snowcone (splash) -> snowfall (login) -> frostedglass (desktop)\n"
                "\n"
                "Options:\n"
                "  --res WxH   Wine desktop resolution (e.g. 1920x1080).\n"
                "              Omit for automatic (uses output native res).\n"
                "\n"
                "Compositor keybindings:\n"
                "  Ctrl+Alt+Backspace   Kill compositor (emergency exit)\n"
                "\n"
                "Everything else (Alt+Tab, Alt+F4, window management) is\n"
                "handled by Wine's Win32 window manager.\n");
            return 0;
        }
    }

    struct fg_server server = {0};
    server.wine_desktop_res = resolution;
    g_server = &server;

    struct sigaction sa = { .sa_handler = sigchld_handler };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    server.display = wl_display_create();
    assert(server.display);

    server.backend = wlr_backend_autocreate(
        wl_display_get_event_loop(server.display), NULL);
    assert(server.backend);

    server.renderer = wlr_renderer_autocreate(server.backend);
    assert(server.renderer);
    wlr_renderer_init_wl_display(server.renderer, server.display);

    server.allocator =
        wlr_allocator_autocreate(server.backend, server.renderer);
    assert(server.allocator);

    wlr_compositor_create(server.display, 6, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    server.output_layout = wlr_output_layout_create(server.display);
    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.scene = wlr_scene_create();
    server.scene_layout =
        wlr_scene_attach_output_layout(server.scene, server.output_layout);

    wl_list_init(&server.toplevels);
    server.xdg_shell = wlr_xdg_shell_create(server.display, 6);
    server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel,
        &server.new_xdg_toplevel);
    server.new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup,
        &server.new_xdg_popup);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

    server.cursor_motion.notify = cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute,
        &server.cursor_motion_absolute);
    server.cursor_button.notify = cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    wl_list_init(&server.keyboards);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    server.seat = wlr_seat_create(server.display, "seat0");
    server.request_set_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor,
        &server.request_set_cursor);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection,
        &server.request_set_selection);

    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_log(WLR_ERROR, "Failed to open Wayland socket");
        wlr_backend_destroy(server.backend);
        return 1;
    }

    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "Failed to start backend");
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.display);
        return 1;
    }

    wlr_log(WLR_INFO, "frostedglass running on Wayland display %s", socket);

    launch_wine(&server, socket);

    wl_display_run(server.display);

    if (server.wine_pid > 0) {
        kill(server.wine_pid, SIGTERM);
        waitpid(server.wine_pid, NULL, 0);
    }

    wl_display_destroy_clients(server.display);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_output_layout_destroy(server.output_layout);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);

    wlr_log(WLR_INFO, "frostedglass shut down cleanly");
    return 0;
}
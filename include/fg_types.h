/*
 * fg_types.h — Shared types for the frostedglass compositor.
 *
 * All compositor structures and enums live here so that every
 * translation unit sees the same layout without circular includes.
 */

#ifndef FG_TYPES_H
#define FG_TYPES_H

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>

/* ------------------------------------------------------------------ */
/* Enums                                                              */
/* ------------------------------------------------------------------ */

enum fg_cursor_mode {
    FG_CURSOR_PASSTHROUGH,
    FG_CURSOR_MOVE,
    FG_CURSOR_RESIZE,
};

/* ------------------------------------------------------------------ */
/* Core server state                                                  */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Per-output state                                                   */
/* ------------------------------------------------------------------ */

struct fg_output {
    struct wl_list link;
    struct fg_server *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
};

/* ------------------------------------------------------------------ */
/* Per-toplevel (window) state                                        */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Per-popup state (menus, tooltips, dropdowns)                       */
/* ------------------------------------------------------------------ */

struct fg_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

/* ------------------------------------------------------------------ */
/* Per-keyboard state                                                 */
/* ------------------------------------------------------------------ */

struct fg_keyboard {
    struct wl_list link;
    struct fg_server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

/* ------------------------------------------------------------------ */
/* wlroots version compatibility                                      */
/* ------------------------------------------------------------------ */

/*
 * wlr_xdg_surface_get_geometry() was removed in wlroots 0.19+.
 * The geometry is now read directly from the struct field.
 * WLR_VERSION_MINOR is defined in <wlr/version.h> (0.18+).
 */
#include <wlr/version.h>

static inline void fg_xdg_surface_get_geometry(
    struct wlr_xdg_surface *surface, struct wlr_box *box) {
#if WLR_VERSION_MINOR < 19
    wlr_xdg_surface_get_geometry(surface, box);
#else
    *box = surface->geometry;
#endif
}

#endif /* FG_TYPES_H */
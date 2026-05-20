/*
 * fg_toplevel.c — XDG toplevel, popup, and focus management.
 *
 * Handles the lifecycle of every xdg_toplevel (one per Win32 window
 * from Wine) and xdg_popup (menus, tooltips, dropdowns).  Also owns
 * the focus_toplevel() and toplevel_at() helpers used by input code.
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "fg_toplevel.h"
#include "fg_output.h"   /* struct fg_output — for screen dimensions */

/* ------------------------------------------------------------------ */
/* Focus                                                              */
/* ------------------------------------------------------------------ */

void focus_toplevel(struct fg_toplevel *toplevel) {
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

    /*
     * The taskbar must always be the topmost node in the scene graph
     * so it renders above all windows (like Windows' "always on top"
     * taskbar behavior).  If we just raised a non-taskbar window,
     * re-raise the taskbar above it.
     */
    if (server->taskbar && toplevel != server->taskbar &&
        server->taskbar->xdg_toplevel->base->surface->mapped) {
        wlr_scene_node_raise_to_top(
            &server->taskbar->scene_tree->node);
    }

    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

    struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
    if (kb) {
        wlr_seat_keyboard_notify_enter(seat, surface,
            kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }
}

struct fg_toplevel *toplevel_at(struct fg_server *server,
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

/* ------------------------------------------------------------------ */
/* Interactive move / resize                                          */
/* ------------------------------------------------------------------ */

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
        fg_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
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

/* ------------------------------------------------------------------ */
/* Taskbar positioning                                                */
/* ------------------------------------------------------------------ */

/*
 * Check if a toplevel is Wine's taskbar (Shell_TrayWnd) and, if so,
 * force it to the bottom of the screen.  This mirrors what WSLK did
 * via labwc window rules — Wine's own StuckRects2 registry hint is
 * unreliable under Wayland, so the compositor has to enforce it.
 *
 * Wine's Wayland driver sets app_id to the Win32 window class name,
 * so the taskbar is identified by app_id == "Shell_TrayWnd".
 */
static bool is_taskbar(struct wlr_xdg_toplevel *xdg_toplevel) {
    const char *app_id = xdg_toplevel->app_id;
    if (!app_id) return false;

    /* Wine may use the exact class name or lowercase variants */
    if (strcmp(app_id, "Shell_TrayWnd") == 0) return true;
    if (strcmp(app_id, "shell_traywnd") == 0) return true;
    if (strcmp(app_id, "explorer.exe") == 0) return true;

    return false;
}

static void position_taskbar(struct fg_toplevel *toplevel) {
    struct fg_server *server = toplevel->server;

    /* Register this toplevel as THE taskbar */
    server->taskbar = toplevel;

    int screen_w, screen_h;
    if (!server_get_screen_size(server, &screen_w, &screen_h)) return;

    /* Get the taskbar's own height from its geometry */
    struct wlr_box geo;
    fg_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
    int bar_h = geo.height > 0 ? geo.height : 30;

    int target_y = screen_h - bar_h;

    wlr_log(WLR_INFO,
        "Taskbar detected (app_id=%s, title=%s): "
        "screen=%dx%d, bar_h=%d, placing at y=%d",
        toplevel->xdg_toplevel->app_id ?: "(null)",
        toplevel->xdg_toplevel->title ?: "(null)",
        screen_w, screen_h, bar_h, target_y);

    /* Position at bottom, full width */
    wlr_scene_node_set_position(&toplevel->scene_tree->node,
        0, target_y);

    /* Tell Wine the taskbar should span the full screen width.
     * This is a hint — Wine may or may not respect it, but it
     * helps when Wine's own StuckRects2 doesn't have the right
     * screen dimensions yet. */
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, screen_w, bar_h);

    /* Taskbar is always on top of other windows in z-order */
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
}

/*
 * Get the height of the taskbar, or 0 if no taskbar is mapped.
 * Used to compute the work area for window positioning.
 */
static int get_taskbar_height(struct fg_server *server) {
    if (!server->taskbar) return 0;
    if (!server->taskbar->xdg_toplevel->base->surface->mapped) return 0;

    struct wlr_box geo;
    fg_xdg_surface_get_geometry(server->taskbar->xdg_toplevel->base, &geo);
    return geo.height > 0 ? geo.height : 30;
}

/* ------------------------------------------------------------------ */
/* Per-toplevel listener callbacks                                    */
/* ------------------------------------------------------------------ */

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, map);
    struct fg_server *server = toplevel->server;

    wl_list_insert(&server->toplevels, &toplevel->link);

    /* Log every mapped surface so we can see what Wine sends */
    wlr_log(WLR_INFO, "Toplevel mapped: app_id=%s title=%s pos=(%d,%d)",
        toplevel->xdg_toplevel->app_id ?: "(null)",
        toplevel->xdg_toplevel->title ?: "(null)",
        toplevel->scene_tree->node.x,
        toplevel->scene_tree->node.y);

    if (is_taskbar(toplevel->xdg_toplevel)) {
        position_taskbar(toplevel);
        /* Don't focus the taskbar — it would steal focus from apps */
        return;
    }

    /*
     * Wine's Wayland driver positions windows based on Win32
     * coordinates.  We mostly respect Wine's placement, but enforce
     * one rule: non-fullscreen windows must not overlap the taskbar.
     *
     * If a window's bottom edge would extend into the taskbar area,
     * nudge it upward.  This handles cases like the Run dialog which
     * Wine places near the taskbar.
     */
    int screen_w, screen_h;
    if (server_get_screen_size(server, &screen_w, &screen_h)) {
        int bar_h = get_taskbar_height(server);
        int work_h = screen_h - bar_h;

        struct wlr_box geo;
        fg_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);

        int node_x = toplevel->scene_tree->node.x;
        int node_y = toplevel->scene_tree->node.y;
        int win_bottom = node_y + geo.height;

        if (geo.height > 0 && win_bottom > work_h) {
            /* Push the window up so its bottom edge sits at the
             * top of the taskbar */
            int new_y = work_h - geo.height;
            if (new_y < 0) new_y = 0;

            wlr_log(WLR_INFO,
                "Clamping window above taskbar: y=%d -> %d "
                "(win_h=%d, work_h=%d)",
                node_y, new_y, geo.height, work_h);

            wlr_scene_node_set_position(
                &toplevel->scene_tree->node, node_x, new_y);
        }
    }

    focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, unmap);
    struct fg_server *server = toplevel->server;

    if (toplevel == server->grabbed_toplevel) {
        server->cursor_mode = FG_CURSOR_PASSTHROUGH;
        server->grabbed_toplevel = NULL;
    }

    /*
     * Note: we do NOT clear server->taskbar here.  Wine may unmap and
     * re-map the taskbar (e.g. when it reconfigures itself).  We keep
     * the pointer so that when it re-maps, position_taskbar() will
     * re-register it.  The pointer is only cleared on destroy.
     */

    wl_list_remove(&toplevel->link);
    wl_list_init(&toplevel->link);  /* safe for double-remove */
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, commit);
    struct fg_server *server = toplevel->server;

    if (toplevel->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        return;
    }

    /*
     * If this is the taskbar and it's mapped, reposition it on every
     * commit.  Wine may resize the taskbar after the initial map (e.g.
     * when it discovers the real screen resolution), and we need to
     * keep it pinned to the bottom.
     */
    if (server->taskbar == toplevel &&
        toplevel->xdg_toplevel->base->surface->mapped) {
        position_taskbar(toplevel);
        return;
    }

    /*
     * For non-taskbar, non-fullscreen windows: enforce work-area
     * clamping on every commit.  Wine repositions windows via
     * subsequent surface commits (Win32 SetWindowPos → Wayland
     * commit), so we must clamp here too, not just on map.
     */
    if (!toplevel->xdg_toplevel->base->surface->mapped) return;
    if (toplevel->xdg_toplevel->current.fullscreen) return;

    int screen_w, screen_h;
    if (!server_get_screen_size(server, &screen_w, &screen_h)) return;

    int bar_h = get_taskbar_height(server);
    if (bar_h <= 0) return;

    int work_h = screen_h - bar_h;

    struct wlr_box geo;
    fg_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
    if (geo.height <= 0) return;

    int node_y = toplevel->scene_tree->node.y;
    int win_bottom = node_y + geo.height;

    if (win_bottom > work_h) {
        int new_y = work_h - geo.height;
        if (new_y < 0) new_y = 0;
        wlr_scene_node_set_position(&toplevel->scene_tree->node,
            toplevel->scene_tree->node.x, new_y);
    }
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, destroy);

    /* Clear taskbar tracking if this was the taskbar */
    if (toplevel->server->taskbar == toplevel) {
        toplevel->server->taskbar = NULL;
        wlr_log(WLR_INFO, "Taskbar toplevel destroyed");
    }

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
    struct fg_server *server = toplevel->server;

    if (!toplevel->xdg_toplevel->base->surface->mapped) return;

    if (toplevel->xdg_toplevel->requested.maximized) {
        /* Maximize into the work area (above the taskbar) */
        int screen_w, screen_h;
        if (server_get_screen_size(server, &screen_w, &screen_h)) {
            int bar_h = get_taskbar_height(server);
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
                screen_w, screen_h - bar_h);
            wlr_scene_node_set_position(
                &toplevel->scene_tree->node, 0, 0);
        }
        wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
    } else {
        wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
        /* Let Wine handle restoring to the previous size/position */
    }
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener,
    void *data) {
    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_fullscreen);
    struct fg_server *server = toplevel->server;

    if (!toplevel->xdg_toplevel->base->surface->mapped) return;

    if (toplevel->xdg_toplevel->requested.fullscreen) {
        int screen_w, screen_h;
        if (server_get_screen_size(server, &screen_w, &screen_h)) {
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
                screen_w, screen_h);
            wlr_scene_node_set_position(
                &toplevel->scene_tree->node, 0, 0);
            /* Raise above taskbar for true fullscreen */
            wlr_scene_node_raise_to_top(
                &toplevel->scene_tree->node);
        }
        wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true);
    } else {
        wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, false);
        /* Restore taskbar on top */
        if (server->taskbar &&
            server->taskbar->xdg_toplevel->base->surface->mapped) {
            wlr_scene_node_raise_to_top(
                &server->taskbar->scene_tree->node);
        }
    }
}

/* ------------------------------------------------------------------ */
/* New toplevel from xdg_shell                                        */
/* ------------------------------------------------------------------ */

void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
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

/* ------------------------------------------------------------------ */
/* Per-popup listener callbacks                                       */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* New popup from xdg_shell                                           */
/* ------------------------------------------------------------------ */

void server_new_xdg_popup(struct wl_listener *listener, void *data) {
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
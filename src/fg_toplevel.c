/*
 * fg_toplevel.c — XDG toplevel, popup, and focus management.
 *
 * Handles the lifecycle of every xdg_toplevel (one per Win32 window
 * from Wine) and xdg_popup (menus, tooltips, dropdowns).  Also owns
 * focus_toplevel() and toplevel_at() used by input code.
 *
 * Taskbar detection/positioning lives in fg_taskbar.c.
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "fg_toplevel.h"
#include "fg_taskbar.h"
#include "fg_output.h"

/* ------------------------------------------------------------------ */
/* Focus                                                              */
/* ------------------------------------------------------------------ */

void focus_toplevel(struct fg_toplevel *toplevel) {
    if (!toplevel) return;

    struct fg_server *server = toplevel->server;
    struct wlr_seat *seat = server->seat;

    if (!toplevel->xdg_toplevel ||
        !toplevel->xdg_toplevel->base ||
        !toplevel->xdg_toplevel->base->surface ||
        !toplevel->xdg_toplevel->base->surface->mapped)
        return;

    struct wlr_surface *prev_surface =
        seat->keyboard_state.focused_surface;

    struct wlr_surface *surface =
        toplevel->xdg_toplevel->base->surface;

    if (prev_surface == surface) return;

    /*
     * Deactivate the previous toplevel only if still mapped.
     * Avoid configure events racing destroyed Wine HWNDs.
     */
    if (prev_surface && prev_surface->mapped) {
        struct wlr_xdg_toplevel *prev =
            wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);

        if (prev && prev->base &&
            prev->base->surface &&
            prev->base->surface->mapped) {
            wlr_xdg_toplevel_set_activated(prev, false);
        }
    }

    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);

    /*
     * Keep MRU ordering.
     *
     * Only manipulate the list if linked. During teardown/unmap the
     * node may already have been removed.
     */
    if (!wl_list_empty(&toplevel->link)) {
        wl_list_remove(&toplevel->link);
        wl_list_insert(&server->toplevels, &toplevel->link);
    }

    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

    struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);

    if (kb) {
        wlr_seat_keyboard_notify_enter(
            seat,
            surface,
            kb->keycodes,
            kb->num_keycodes,
            &kb->modifiers);
    }
}

/*
 * Transfer focus to the next available toplevel.
 *
 * IMPORTANT:
 * Never transiently clear keyboard focus if another mapped surface
 * exists. Wine interprets empty focus states as session teardown.
 */
static void refocus_next(struct fg_server *server,
    struct fg_toplevel *exclude) {

    struct fg_toplevel *next;

    /* Pass 1: prefer non-taskbar windows */
    wl_list_for_each(next, &server->toplevels, link) {
        if (next == exclude)
            continue;

        if (next == server->taskbar)
            continue;

        if (!next->xdg_toplevel ||
            !next->xdg_toplevel->base ||
            !next->xdg_toplevel->base->surface)
            continue;

        if (next->xdg_toplevel->base->surface->mapped) {
            focus_toplevel(next);
            return;
        }
    }

    /* Pass 2: fallback to taskbar */
    if (server->taskbar &&
        server->taskbar != exclude &&
        server->taskbar->xdg_toplevel &&
        server->taskbar->xdg_toplevel->base &&
        server->taskbar->xdg_toplevel->base->surface &&
        server->taskbar->xdg_toplevel->base->surface->mapped) {

        focus_toplevel(server->taskbar);
        return;
    }

    /*
     * ONLY clear focus if literally nothing remains.
     */
    wlr_seat_keyboard_clear_focus(server->seat);
}

/* ------------------------------------------------------------------ */
/* Hit-testing                                                        */
/* ------------------------------------------------------------------ */

struct fg_toplevel *toplevel_at(struct fg_server *server,
    double lx, double ly, struct wlr_surface **surface,
    double *sx, double *sy) {

    struct wlr_scene_node *node =
        wlr_scene_node_at(
            &server->scene->tree.node,
            lx, ly, sx, sy);

    if (!node || node->type != WLR_SCENE_NODE_BUFFER)
        return NULL;

    struct wlr_scene_buffer *scene_buffer =
        wlr_scene_buffer_from_node(node);

    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(scene_buffer);

    if (!scene_surface)
        return NULL;

    *surface = scene_surface->surface;

    struct wlr_scene_tree *tree = node->parent;

    while (tree && !tree->node.data)
        tree = tree->node.parent;

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
        server->grab_x =
            server->cursor->x -
            toplevel->scene_tree->node.x;

        server->grab_y =
            server->cursor->y -
            toplevel->scene_tree->node.y;
    } else {
        struct wlr_box geo;

        fg_xdg_surface_get_geometry(
            toplevel->xdg_toplevel->base,
            &geo);

        double bx =
            (toplevel->scene_tree->node.x + geo.x) +
            ((edges & WLR_EDGE_RIGHT) ? geo.width : 0);

        double by =
            (toplevel->scene_tree->node.y + geo.y) +
            ((edges & WLR_EDGE_BOTTOM) ? geo.height : 0);

        server->grab_x = server->cursor->x - bx;
        server->grab_y = server->cursor->y - by;

        server->grab_geobox = geo;
        server->grab_geobox.x +=
            toplevel->scene_tree->node.x;

        server->grab_geobox.y +=
            toplevel->scene_tree->node.y;

        server->resize_edges = edges;
    }
}

/* ------------------------------------------------------------------ */
/* Centering helper                                                   */
/* ------------------------------------------------------------------ */

static bool try_center(struct fg_toplevel *toplevel) {
    if (toplevel->centered)
        return false;

    struct fg_server *server = toplevel->server;

    int screen_w, screen_h;

    if (!server_get_screen_size(server, &screen_w, &screen_h))
        return false;

    int bar_h = get_taskbar_height(server);
    int work_h = screen_h - bar_h;

    if (work_h <= 0)
        work_h = screen_h;

    struct wlr_box geo;

    fg_xdg_surface_get_geometry(
        toplevel->xdg_toplevel->base,
        &geo);

    if (geo.width <= 0 || geo.height <= 0)
        return false;

    int nx = toplevel->scene_tree->node.x;
    int ny = toplevel->scene_tree->node.y;

    if (nx == 0 &&
        ny == 0 &&
        geo.width < screen_w &&
        geo.height < work_h) {

        nx = (screen_w - geo.width) / 2;
        ny = (work_h - geo.height) / 2;

        wlr_scene_node_set_position(
            &toplevel->scene_tree->node,
            nx, ny);

        toplevel->centered = true;

        wlr_log(WLR_INFO,
            "Centered window: (%d,%d) %dx%d",
            nx, ny, geo.width, geo.height);

        return true;
    }

    toplevel->centered = true;
    return false;
}

static void clamp_above_taskbar(struct fg_toplevel *toplevel) {
    struct fg_server *server = toplevel->server;

    int screen_w, screen_h;

    if (!server_get_screen_size(server, &screen_w, &screen_h))
        return;

    int bar_h = get_taskbar_height(server);

    if (bar_h <= 0)
        return;

    int work_h = screen_h - bar_h;

    struct wlr_box geo;

    fg_xdg_surface_get_geometry(
        toplevel->xdg_toplevel->base,
        &geo);

    if (geo.height <= 0)
        return;

    int ny = toplevel->scene_tree->node.y;

    if (ny + geo.height > work_h) {
        int new_y = work_h - geo.height;

        if (new_y < 0)
            new_y = 0;

        wlr_scene_node_set_position(
            &toplevel->scene_tree->node,
            toplevel->scene_tree->node.x,
            new_y);
    }
}

/* ------------------------------------------------------------------ */
/* Toplevel lifecycle                                                 */
/* ------------------------------------------------------------------ */

static void xdg_toplevel_map(struct wl_listener *listener,
    void *data) {

    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, map);

    struct fg_server *server = toplevel->server;

    wl_list_insert(&server->toplevels, &toplevel->link);

    wlr_log(WLR_INFO,
        "Toplevel mapped: app_id=%s title=\"%s\"",
        toplevel->xdg_toplevel->app_id ?: "(null)",
        toplevel->xdg_toplevel->title ?: "");

    if (is_taskbar(toplevel)) {
        position_taskbar(toplevel);

        wlr_scene_node_raise_to_top(
            &toplevel->scene_tree->node);

        wlr_log(WLR_INFO,
            "Taskbar pinned to bottom on map");

        return;
    }

    try_center(toplevel);
    clamp_above_taskbar(toplevel);
    focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener,
    void *data) {

    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, unmap);

    struct fg_server *server = toplevel->server;

    wlr_log(WLR_INFO,
        "Toplevel unmapped: app_id=%s title=\"%s\"",
        toplevel->xdg_toplevel->app_id ?: "(null)",
        toplevel->xdg_toplevel->title ?: "");

    if (toplevel == server->grabbed_toplevel) {
        server->cursor_mode = FG_CURSOR_PASSTHROUGH;
        server->grabbed_toplevel = NULL;
    }

    /*
     * Remove from MRU/toplevel list exactly once.
     */
    if (!wl_list_empty(&toplevel->link)) {
        wl_list_remove(&toplevel->link);
        wl_list_init(&toplevel->link);
    }

    /*
     * Refocus another mapped window immediately.
     */
    refocus_next(server, toplevel);
}

static void xdg_toplevel_commit(struct wl_listener *listener,
    void *data) {

    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, commit);

    struct fg_server *server = toplevel->server;

    if (toplevel->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(
            toplevel->xdg_toplevel,
            0, 0);

        return;
    }

    if (!toplevel->xdg_toplevel->base->surface->mapped)
        return;

    if (server->taskbar == toplevel) {
        position_taskbar(toplevel);
        return;
    }

    try_detect_taskbar(toplevel);

    if (server->taskbar == toplevel)
        return;

    if (toplevel->xdg_toplevel->current.fullscreen)
        return;

    try_center(toplevel);
    clamp_above_taskbar(toplevel);
}

static void xdg_toplevel_destroy(struct wl_listener *listener,
    void *data) {

    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, destroy);

    struct fg_server *server = toplevel->server;

    wlr_log(WLR_INFO,
        "Toplevel destroyed: app_id=%s",
        toplevel->xdg_toplevel->app_id ?: "(null)");

    if (toplevel == server->grabbed_toplevel) {
        server->cursor_mode = FG_CURSOR_PASSTHROUGH;
        server->grabbed_toplevel = NULL;
    }

    if (server->taskbar == toplevel) {
        server->taskbar = NULL;

        wlr_log(WLR_INFO,
            "Taskbar destroyed");
    }

    /*
     * DO NOT wl_list_remove(&toplevel->link) here.
     *
     * The node was already removed during unmap.
     * Double-removal corrupts wl_list and can kill
     * the entire Wine session.
     */

    /*
     * Only refocus if no focused surface exists.
     * Avoid duplicate focus churn during teardown.
     */
    if (!server->seat->keyboard_state.focused_surface) {
        refocus_next(server, toplevel);
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

/* ------------------------------------------------------------------ */
/* Request handlers                                                   */
/* ------------------------------------------------------------------ */

static void xdg_toplevel_request_move(
    struct wl_listener *listener,
    void *data) {

    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_move);

    begin_interactive(
        toplevel,
        FG_CURSOR_MOVE,
        0);
}

static void xdg_toplevel_request_resize(
    struct wl_listener *listener,
    void *data) {

    struct wlr_xdg_toplevel_resize_event *event = data;

    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_resize);

    begin_interactive(
        toplevel,
        FG_CURSOR_RESIZE,
        event->edges);
}

static void xdg_toplevel_request_maximize(
    struct wl_listener *listener,
    void *data) {

    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_maximize);

    struct fg_server *server = toplevel->server;

    if (!toplevel->xdg_toplevel->base->surface->mapped)
        return;

    if (toplevel->xdg_toplevel->requested.maximized) {
        int screen_w, screen_h;

        if (server_get_screen_size(server,
                &screen_w, &screen_h)) {

            int bar_h = get_taskbar_height(server);

            wlr_xdg_toplevel_set_size(
                toplevel->xdg_toplevel,
                screen_w,
                screen_h - bar_h);

            wlr_scene_node_set_position(
                &toplevel->scene_tree->node,
                0, 0);
        }

        wlr_xdg_toplevel_set_maximized(
            toplevel->xdg_toplevel,
            true);
    } else {
        wlr_xdg_toplevel_set_maximized(
            toplevel->xdg_toplevel,
            false);
    }
}

static void xdg_toplevel_request_fullscreen(
    struct wl_listener *listener,
    void *data) {

    struct fg_toplevel *toplevel =
        wl_container_of(listener, toplevel,
            request_fullscreen);

    struct fg_server *server = toplevel->server;

    if (!toplevel->xdg_toplevel->base->surface->mapped)
        return;

    if (toplevel->xdg_toplevel->requested.fullscreen) {
        int screen_w, screen_h;

        if (server_get_screen_size(server,
                &screen_w, &screen_h)) {

            wlr_xdg_toplevel_set_size(
                toplevel->xdg_toplevel,
                screen_w,
                screen_h);

            wlr_scene_node_set_position(
                &toplevel->scene_tree->node,
                0, 0);

            wlr_scene_node_raise_to_top(
                &toplevel->scene_tree->node);
        }

        wlr_xdg_toplevel_set_fullscreen(
            toplevel->xdg_toplevel,
            true);

    } else {
        wlr_xdg_toplevel_set_fullscreen(
            toplevel->xdg_toplevel,
            false);

        if (server->taskbar &&
            server->taskbar->xdg_toplevel->base->surface->mapped) {

            wlr_scene_node_raise_to_top(
                &server->taskbar->scene_tree->node);
        }
    }
}

/* ------------------------------------------------------------------ */
/* New toplevel                                                       */
/* ------------------------------------------------------------------ */

void server_new_xdg_toplevel(struct wl_listener *listener,
    void *data) {

    struct fg_server *server =
        wl_container_of(listener, server,
            new_xdg_toplevel);

    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct fg_toplevel *toplevel =
        calloc(1, sizeof(*toplevel));

    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;

    wl_list_init(&toplevel->link);

    toplevel->scene_tree =
        wlr_scene_xdg_surface_create(
            &server->scene->tree,
            xdg_toplevel->base);

    toplevel->scene_tree->node.data = toplevel;
    xdg_toplevel->base->data = toplevel->scene_tree;

    toplevel->map.notify = xdg_toplevel_map;
    wl_signal_add(
        &xdg_toplevel->base->surface->events.map,
        &toplevel->map);

    toplevel->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(
        &xdg_toplevel->base->surface->events.unmap,
        &toplevel->unmap);

    toplevel->commit.notify = xdg_toplevel_commit;
    wl_signal_add(
        &xdg_toplevel->base->surface->events.commit,
        &toplevel->commit);

    toplevel->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(
        &xdg_toplevel->events.destroy,
        &toplevel->destroy);

    toplevel->request_move.notify =
        xdg_toplevel_request_move;

    wl_signal_add(
        &xdg_toplevel->events.request_move,
        &toplevel->request_move);

    toplevel->request_resize.notify =
        xdg_toplevel_request_resize;

    wl_signal_add(
        &xdg_toplevel->events.request_resize,
        &toplevel->request_resize);

    toplevel->request_maximize.notify =
        xdg_toplevel_request_maximize;

    wl_signal_add(
        &xdg_toplevel->events.request_maximize,
        &toplevel->request_maximize);

    toplevel->request_fullscreen.notify =
        xdg_toplevel_request_fullscreen;

    wl_signal_add(
        &xdg_toplevel->events.request_fullscreen,
        &toplevel->request_fullscreen);
}

/* ------------------------------------------------------------------ */
/* Popups                                                             */
/* ------------------------------------------------------------------ */

static void xdg_popup_commit(struct wl_listener *listener,
    void *data) {

    struct fg_popup *popup =
        wl_container_of(listener, popup, commit);

    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(
            popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(struct wl_listener *listener,
    void *data) {

    struct fg_popup *popup =
        wl_container_of(listener, popup, destroy);

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);

    free(popup);
}

void server_new_xdg_popup(struct wl_listener *listener,
    void *data) {

    struct wlr_xdg_popup *xdg_popup = data;

    struct fg_popup *popup =
        calloc(1, sizeof(*popup));

    popup->xdg_popup = xdg_popup;

    struct wlr_xdg_surface *parent =
        wlr_xdg_surface_try_from_wlr_surface(
            xdg_popup->parent);

    assert(parent);

    struct wlr_scene_tree *parent_tree = parent->data;

    xdg_popup->base->data =
        wlr_scene_xdg_surface_create(
            parent_tree,
            xdg_popup->base);

    popup->commit.notify = xdg_popup_commit;

    wl_signal_add(
        &xdg_popup->base->surface->events.commit,
        &popup->commit);

    popup->destroy.notify = xdg_popup_destroy;

    wl_signal_add(
        &xdg_popup->events.destroy,
        &popup->destroy);
}

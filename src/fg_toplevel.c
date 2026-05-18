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

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "fg_toplevel.h"

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

/* ------------------------------------------------------------------ */
/* Per-toplevel listener callbacks                                    */
/* ------------------------------------------------------------------ */

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

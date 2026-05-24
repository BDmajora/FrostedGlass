#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "fg_toplevel.h"
#include "fg_taskbar.h"
#include "fg_desktop.h"
#include "fg_output.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static bool toplevel_is_mapped(struct fg_toplevel *toplevel) {
    return
        toplevel &&
        toplevel->xdg_toplevel &&
        toplevel->xdg_toplevel->base &&
        toplevel->xdg_toplevel->base->surface &&
        toplevel->xdg_toplevel->base->surface->mapped;
}

static bool toplevel_is_focusable(struct fg_toplevel *toplevel) {
    if (!toplevel_is_mapped(toplevel))
        return false;

    if (!toplevel->scene_tree)
        return false;

    return true;
}

static bool seat_surface_is_alive(struct wlr_surface *surface) {
    return surface != NULL;
}

/* ------------------------------------------------------------------ */
/* Focus                                                              */
/* ------------------------------------------------------------------ */

void focus_toplevel(struct fg_toplevel *toplevel) {
    if (!toplevel)
        return;

    if (!toplevel_is_focusable(toplevel))
        return;

    struct fg_server *server = toplevel->server;

    if (!server || !server->seat)
        return;

    /*
     * Never focus or raise the desktop root window.  It must stay pinned
     * at the bottom of the scene graph.  Clicking the wallpaper should
     * not bring it in front of application windows (which is exactly what
     * raise_to_top below would do) — same as clicking the desktop in real
     * Windows doesn't bury your open windows.
     */
    if (server->desktop == toplevel)
        return;

    struct wlr_seat *seat = server->seat;

    struct wlr_surface *surface =
        toplevel->xdg_toplevel->base->surface;

    if (!seat_surface_is_alive(surface))
        return;

    /*
     * Already focused.
     */
    if (seat->keyboard_state.focused_surface == surface)
        return;

    /*
     * Raise visually first.
     */
    wlr_scene_node_raise_to_top(
        &toplevel->scene_tree->node);

    /*
     * Maintain MRU ordering safely.
     */
    if (!wl_list_empty(&toplevel->link)) {
        wl_list_remove(&toplevel->link);
        wl_list_insert(&server->toplevels,
            &toplevel->link);
    }

    /*
     * CRITICAL:
     *
     * NEVER explicitly deactivate the previous Wine surface.
     *
     * wlroots/wlr_xdg_toplevel_set_activated(false)
     * during teardown can trigger catastrophic Wine
     * session termination.
     *
     * ONLY activate the new surface.
     */
    wlr_xdg_toplevel_set_activated(
        toplevel->xdg_toplevel,
        true);

    struct wlr_keyboard *kb =
        wlr_seat_get_keyboard(seat);

    /*
     * CRITICAL:
     *
     * Only send keyboard enter if a keyboard exists.
     */
    if (!kb)
        return;

    /*
     * CRITICAL:
     *
     * We ONLY ever enter stable mapped surfaces.
     */
    wlr_seat_keyboard_notify_enter(
        seat,
        surface,
        kb->keycodes,
        kb->num_keycodes,
        &kb->modifiers);
}

/*
 * Find safest possible next focus target.
 *
 * NEVER clears focus if ANY mapped surface still exists.
 */
/*
 * CRITICAL — DO NOT TOUCH FOCUS IN THE DESTROY HANDLER.
 *
 * wlroots internally clears keyboard focus when the focused
 * surface is destroyed (via wl_resource destroy listeners on
 * the seat).  We must not interfere:
 *
 * - Activating another surface → sends configure(activated)
 *   to surfaces Wine may be tearing down → Wine freezes.
 *
 * - Clearing focus explicitly → Wine sees no window has focus,
 *   interprets it as "session ending", kills explorer.exe,
 *   which destroys the taskbar and all windows.
 *
 * The correct action is: do nothing.  Let wlroots handle the
 * seat internally.  Wine's Win32 window manager handles focus
 * transitions through its own message pump.
 */

/* ------------------------------------------------------------------ */
/* Hit-testing                                                        */
/* ------------------------------------------------------------------ */

struct fg_toplevel *toplevel_at(
    struct fg_server *server,
    double lx,
    double ly,
    struct wlr_surface **surface,
    double *sx,
    double *sy) {

    struct wlr_scene_node *node =
        wlr_scene_node_at(
            &server->scene->tree.node,
            lx,
            ly,
            sx,
            sy);

    if (!node)
        return NULL;

    if (node->type != WLR_SCENE_NODE_BUFFER)
        return NULL;

    struct wlr_scene_buffer *scene_buffer =
        wlr_scene_buffer_from_node(node);

    if (!scene_buffer)
        return NULL;

    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(
            scene_buffer);

    if (!scene_surface)
        return NULL;

    *surface = scene_surface->surface;

    struct wlr_scene_tree *tree = node->parent;

    while (tree && !tree->node.data)
        tree = tree->node.parent;

    if (!tree)
        return NULL;

    return tree->node.data;
}

/* ------------------------------------------------------------------ */
/* Interactive move / resize                                          */
/* ------------------------------------------------------------------ */

static void begin_interactive(
    struct fg_toplevel *toplevel,
    enum fg_cursor_mode mode,
    uint32_t edges) {

    if (!toplevel_is_focusable(toplevel))
        return;

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

        return;
    }

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

/* ------------------------------------------------------------------ */
/* Centering helper                                                   */
/* ------------------------------------------------------------------ */

static bool try_center(struct fg_toplevel *toplevel) {
    if (!toplevel_is_focusable(toplevel))
        return false;

    if (toplevel->centered)
        return false;

    struct fg_server *server = toplevel->server;

    int screen_w;
    int screen_h;

    if (!server_get_screen_size(
            server,
            &screen_w,
            &screen_h)) {
        return false;
    }

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
            nx,
            ny);

        wlr_log(WLR_INFO,
            "Centered window: (%d,%d) %dx%d",
            nx,
            ny,
            geo.width,
            geo.height);
    }

    toplevel->centered = true;

    return true;
}

static void clamp_above_taskbar(
    struct fg_toplevel *toplevel) {

    if (!toplevel_is_focusable(toplevel))
        return;

    struct fg_server *server = toplevel->server;

    int screen_w;
    int screen_h;

    if (!server_get_screen_size(
            server,
            &screen_w,
            &screen_h)) {
        return;
    }

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

    if (ny + geo.height <= work_h)
        return;

    int new_y = work_h - geo.height;

    if (new_y < 0)
        new_y = 0;

    wlr_scene_node_set_position(
        &toplevel->scene_tree->node,
        toplevel->scene_tree->node.x,
        new_y);
}

/* ------------------------------------------------------------------ */
/* Toplevel lifecycle                                                 */
/* ------------------------------------------------------------------ */

static void xdg_toplevel_map(
    struct wl_listener *listener,
    void *data) {

    (void)data;

    struct fg_toplevel *toplevel =
        wl_container_of(listener,
            toplevel,
            map);

    struct fg_server *server =
        toplevel->server;

    /*
     * Safe insert.
     */
    if (wl_list_empty(&toplevel->link)) {
        wl_list_insert(
            &server->toplevels,
            &toplevel->link);
    }

    wlr_log(WLR_INFO,
        "Toplevel mapped: app_id=%s title=\"%s\"",
        toplevel->xdg_toplevel->app_id ?: "(null)",
        toplevel->xdg_toplevel->title ?: "");

    /*
     * Desktop root window — pin to the bottom and stop.  It must NOT be
     * centered, clamped above the taskbar, or focused: it's the
     * wallpaper layer, not an interactive window.
     */
    if (is_desktop(toplevel)) {

        position_desktop(toplevel);

        wlr_log(WLR_INFO,
            "Desktop pinned to bottom on map");

        return;
    }

    if (is_taskbar(toplevel)) {

        position_taskbar(toplevel);

        wlr_scene_node_raise_to_top(
            &toplevel->scene_tree->node);

        wlr_log(WLR_INFO,
            "Taskbar pinned to bottom on map");

        return;
    }

    /*
     * If we have no taskbar, any new map is a chance to find one
     * (Wine may have respawned explorer).
     */
    if (!server->taskbar) {
        taskbar_rescan_all(server);
    }

    /*
     * Likewise, if we lost the desktop, a new map is a chance to
     * recover it.
     */
    if (!server->desktop) {
        desktop_rescan_all(server);
    }

    try_center(toplevel);
    clamp_above_taskbar(toplevel);

    /*
     * Safe to focus here.
     */
    focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(
    struct wl_listener *listener,
    void *data) {

    (void)data;

    struct fg_toplevel *toplevel =
        wl_container_of(listener,
            toplevel,
            unmap);

    struct fg_server *server =
        toplevel->server;

    wlr_log(WLR_INFO,
        "Toplevel unmapped: app_id=%s title=\"%s\"",
        toplevel->xdg_toplevel->app_id ?: "(null)",
        toplevel->xdg_toplevel->title ?: "");

    /*
     * Taskbar unmap — the surface is gone but the toplevel
     * still exists.  Clear the pointer NOW so that commit
     * handlers on other surfaces can re-detect immediately,
     * rather than pointing at a stale unmapped surface until
     * destroy fires (which may be much later).
     */
    if (server->taskbar == toplevel) {
        server->taskbar = NULL;

        wlr_log(WLR_INFO,
            "Taskbar unmapped — scanning for replacement");

        taskbar_rescan_all(server);
    }

    /*
     * Desktop unmap — same treatment as the taskbar.  Clear the pointer
     * now so commit handlers can re-detect a replacement immediately
     * rather than holding a stale unmapped surface until destroy.
     */
    if (server->desktop == toplevel) {
        server->desktop = NULL;

        wlr_log(WLR_INFO,
            "Desktop unmapped — scanning for replacement");

        desktop_rescan_all(server);
    }

    /*
     * Cancel interactive operations.
     */
    if (toplevel == server->grabbed_toplevel) {
        server->cursor_mode =
            FG_CURSOR_PASSTHROUGH;

        server->grabbed_toplevel = NULL;
    }

    /*
     * CRITICAL:
     *
     * Remove from MRU exactly once.
     */
    if (!wl_list_empty(&toplevel->link)) {
        wl_list_remove(&toplevel->link);
        wl_list_init(&toplevel->link);
    }

    /*
     * ABSOLUTELY NEVER REFOCUS HERE.
     *
     * Refocus from unmap() is the exact bug
     * that kills all Wine windows.
     */
}

static void xdg_toplevel_commit(
    struct wl_listener *listener,
    void *data) {

    (void)data;

    struct fg_toplevel *toplevel =
        wl_container_of(listener,
            toplevel,
            commit);

    struct fg_server *server =
        toplevel->server;

    if (toplevel->xdg_toplevel->base->initial_commit) {

        wlr_xdg_toplevel_set_size(
            toplevel->xdg_toplevel,
            0,
            0);

        return;
    }

    if (!toplevel_is_mapped(toplevel))
        return;

    /*
     * Desktop root window — re-assert bottom pin on every commit
     * (idempotent) and stop.  Must skip taskbar detection, centering,
     * and clamping.
     */
    if (server->desktop == toplevel) {
        position_desktop(toplevel);
        return;
    }

    if (server->taskbar == toplevel) {
        position_taskbar(toplevel);
        return;
    }

    /*
     * Not yet identified — give both detectors a chance.  Desktop first
     * (it's the more fundamental layer), then taskbar.
     */
    try_detect_desktop(toplevel);

    if (server->desktop == toplevel)
        return;

    try_detect_taskbar(toplevel);

    if (server->taskbar == toplevel)
        return;

    if (toplevel->xdg_toplevel->current.fullscreen)
        return;

    try_center(toplevel);
    clamp_above_taskbar(toplevel);
}

static void xdg_toplevel_destroy(
    struct wl_listener *listener,
    void *data) {

    (void)data;

    struct fg_toplevel *toplevel =
        wl_container_of(listener,
            toplevel,
            destroy);

    struct fg_server *server =
        toplevel->server;

    wlr_log(WLR_INFO,
        "Toplevel destroyed: app_id=%s title=\"%s\" is_taskbar=%d",
        toplevel->xdg_toplevel->app_id ?: "(null)",
        toplevel->xdg_toplevel->title ?: "",
        (server->taskbar == toplevel));

    /*
     * Cancel grabs safely.
     */
    if (toplevel == server->grabbed_toplevel) {
        server->cursor_mode =
            FG_CURSOR_PASSTHROUGH;

        server->grabbed_toplevel = NULL;
    }

    /*
     * Taskbar cleanup — clear and immediately try to
     * re-acquire from remaining mapped toplevels.
     * Wine may have already remapped a replacement
     * explorer surface before the old one is destroyed.
     */
    if (server->taskbar == toplevel) {
        server->taskbar = NULL;

        wlr_log(WLR_INFO,
            "Taskbar destroyed — scanning for replacement");

        taskbar_rescan_all(server);
    }

    /*
     * Desktop cleanup — clear and try to re-acquire from remaining
     * mapped toplevels.  This involves no focus changes (the desktop is
     * never focused), so it's safe to do in the destroy handler unlike
     * the focus operations forbidden below.
     */
    if (server->desktop == toplevel) {
        server->desktop = NULL;

        wlr_log(WLR_INFO,
            "Desktop destroyed — scanning for replacement");

        desktop_rescan_all(server);
    }

    /*
     * CRITICAL:
     *
     * NEVER wl_list_remove(&toplevel->link)
     * here.
     *
     * unmap() already removed it.
     *
     * Double remove corrupts wl_list.
     */

    /*
     * Do NOT touch focus here.  See comment above.
     * wlroots handles seat cleanup internally.
     */

    /*
     * Re-raise taskbar if it exists — window destruction can
     * leave it buried in the scene graph.
     */
    if (server->taskbar &&
        server->taskbar != toplevel &&
        server->taskbar->xdg_toplevel &&
        server->taskbar->xdg_toplevel->base &&
        server->taskbar->xdg_toplevel->base->surface &&
        server->taskbar->xdg_toplevel->base->surface->mapped) {

        position_taskbar(server->taskbar);
        wlr_scene_node_raise_to_top(
            &server->taskbar->scene_tree->node);
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

    (void)data;

    struct fg_toplevel *toplevel =
        wl_container_of(listener,
            toplevel,
            request_move);

    begin_interactive(
        toplevel,
        FG_CURSOR_MOVE,
        0);
}

static void xdg_toplevel_request_resize(
    struct wl_listener *listener,
    void *data) {

    struct wlr_xdg_toplevel_resize_event *event =
        data;

    struct fg_toplevel *toplevel =
        wl_container_of(listener,
            toplevel,
            request_resize);

    begin_interactive(
        toplevel,
        FG_CURSOR_RESIZE,
        event->edges);
}

static void xdg_toplevel_request_maximize(
    struct wl_listener *listener,
    void *data) {

    (void)data;

    struct fg_toplevel *toplevel =
        wl_container_of(listener,
            toplevel,
            request_maximize);

    struct fg_server *server =
        toplevel->server;

    if (!toplevel_is_mapped(toplevel))
        return;

    if (toplevel->xdg_toplevel->requested.maximized) {

        int screen_w;
        int screen_h;

        if (server_get_screen_size(
                server,
                &screen_w,
                &screen_h)) {

            int bar_h =
                get_taskbar_height(server);

            wlr_xdg_toplevel_set_size(
                toplevel->xdg_toplevel,
                screen_w,
                screen_h - bar_h);

            wlr_scene_node_set_position(
                &toplevel->scene_tree->node,
                0,
                0);
        }

        wlr_xdg_toplevel_set_maximized(
            toplevel->xdg_toplevel,
            true);

        return;
    }

    wlr_xdg_toplevel_set_maximized(
        toplevel->xdg_toplevel,
        false);
}

static void xdg_toplevel_request_fullscreen(
    struct wl_listener *listener,
    void *data) {

    (void)data;

    struct fg_toplevel *toplevel =
        wl_container_of(listener,
            toplevel,
            request_fullscreen);

    struct fg_server *server =
        toplevel->server;

    if (!toplevel_is_mapped(toplevel))
        return;

    if (toplevel->xdg_toplevel->requested.fullscreen) {

        int screen_w;
        int screen_h;

        if (server_get_screen_size(
                server,
                &screen_w,
                &screen_h)) {

            wlr_xdg_toplevel_set_size(
                toplevel->xdg_toplevel,
                screen_w,
                screen_h);

            wlr_scene_node_set_position(
                &toplevel->scene_tree->node,
                0,
                0);

            wlr_scene_node_raise_to_top(
                &toplevel->scene_tree->node);
        }

        wlr_xdg_toplevel_set_fullscreen(
            toplevel->xdg_toplevel,
            true);

        return;
    }

    wlr_xdg_toplevel_set_fullscreen(
        toplevel->xdg_toplevel,
        false);

    if (server->taskbar &&
        toplevel_is_mapped(server->taskbar)) {

        wlr_scene_node_raise_to_top(
            &server->taskbar->scene_tree->node);
    }
}

/* ------------------------------------------------------------------ */
/* New toplevel                                                       */
/* ------------------------------------------------------------------ */

void server_new_xdg_toplevel(
    struct wl_listener *listener,
    void *data) {

    struct fg_server *server =
        wl_container_of(listener,
            server,
            new_xdg_toplevel);

    struct wlr_xdg_toplevel *xdg_toplevel =
        data;

    struct fg_toplevel *toplevel =
        calloc(1, sizeof(*toplevel));

    if (!toplevel)
        return;

    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;

    wl_list_init(&toplevel->link);

    toplevel->scene_tree =
        wlr_scene_xdg_surface_create(
            &server->scene->tree,
            xdg_toplevel->base);

    if (!toplevel->scene_tree) {
        free(toplevel);
        return;
    }

    toplevel->scene_tree->node.data =
        toplevel;

    xdg_toplevel->base->data =
        toplevel->scene_tree;

    toplevel->map.notify =
        xdg_toplevel_map;

    wl_signal_add(
        &xdg_toplevel->base->surface->events.map,
        &toplevel->map);

    toplevel->unmap.notify =
        xdg_toplevel_unmap;

    wl_signal_add(
        &xdg_toplevel->base->surface->events.unmap,
        &toplevel->unmap);

    toplevel->commit.notify =
        xdg_toplevel_commit;

    wl_signal_add(
        &xdg_toplevel->base->surface->events.commit,
        &toplevel->commit);

    toplevel->destroy.notify =
        xdg_toplevel_destroy;

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

static void xdg_popup_commit(
    struct wl_listener *listener,
    void *data) {

    (void)data;

    struct fg_popup *popup =
        wl_container_of(listener,
            popup,
            commit);

    /*
     * Initial configure required by protocol.
     */
    if (popup->xdg_popup->base->initial_commit) {

        wlr_xdg_surface_schedule_configure(
            popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(
    struct wl_listener *listener,
    void *data) {

    (void)data;

    struct fg_popup *popup =
        wl_container_of(listener,
            popup,
            destroy);

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);

    free(popup);
}

void server_new_xdg_popup(
    struct wl_listener *listener,
    void *data) {

    (void)listener;

    struct wlr_xdg_popup *xdg_popup =
        data;

    struct fg_popup *popup =
        calloc(1, sizeof(*popup));

    if (!popup)
        return;

    popup->xdg_popup = xdg_popup;

    struct wlr_xdg_surface *parent =
        wlr_xdg_surface_try_from_wlr_surface(
            xdg_popup->parent);

    /*
     * CRITICAL:
     *
     * Never assert-crash compositor because of a
     * transient/destroyed Wine popup parent.
     */
    if (!parent) {
        free(popup);
        return;
    }

    struct wlr_scene_tree *parent_tree =
        parent->data;

    /*
     * Parent may already be tearing down.
     */
    if (!parent_tree) {
        free(popup);
        return;
    }

    xdg_popup->base->data =
        wlr_scene_xdg_surface_create(
            parent_tree,
            xdg_popup->base);

    /*
     * Scene creation may fail during teardown races.
     */
    if (!xdg_popup->base->data) {
        free(popup);
        return;
    }

    popup->commit.notify =
        xdg_popup_commit;

    wl_signal_add(
        &xdg_popup->base->surface->events.commit,
        &popup->commit);

    popup->destroy.notify =
        xdg_popup_destroy;

    wl_signal_add(
        &xdg_popup->events.destroy,
        &popup->destroy);
}
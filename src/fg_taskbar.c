/*
 * fg_taskbar.c — Taskbar detection and bottom-pinning for frostedglass.
 *
 * Wine's Wayland driver sets app_id to the PROCESS name ("explorer.exe"),
 * not the Win32 window class ("Shell_TrayWnd").  Every window created by
 * explorer.exe — taskbar, Run dialog, file browser — shares the same
 * app_id.  We detect the taskbar by geometry: the taskbar is the
 * explorer.exe window that spans the full screen width with a small height.
 */

#define _POSIX_C_SOURCE 200809L

#include <string.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "fg_taskbar.h"
#include "fg_output.h"
#include "fg_wine.h"

/* ------------------------------------------------------------------ */
/* Detection                                                          */
/* ------------------------------------------------------------------ */

bool is_taskbar(struct fg_toplevel *toplevel) {
    /* Already tagged — fast path */
    if (toplevel->server->taskbar == toplevel) return true;

    const char *app_id = toplevel->xdg_toplevel->app_id;
    if (!app_id) return false;

    /*
     * Must come from explorer.exe.  Wine lowercases the process name,
     * so we check both forms defensively.
     */
    if (strcmp(app_id, "explorer.exe") != 0 &&
        strcmp(app_id, "Explorer.exe") != 0 &&
        strcmp(app_id, "EXPLORER.EXE") != 0)
        return false;

    /*
     * Geometry heuristic: the taskbar spans (nearly) the full screen
     * width and is short.  Other explorer.exe windows (Run dialog,
     * file browser, Control Panel) are much narrower or much taller.
     *
     * Thresholds:
     *   width  >= 80% of screen width  (taskbar is full-width)
     *   height <  120 pixels            (taskbar is thin)
     *   height >  0                     (geometry must be valid)
     */
    struct wlr_box geo;
    fg_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
    if (geo.width <= 0 || geo.height <= 0) return false;
    if (geo.height >= 120) return false;

    int screen_w, screen_h;
    if (!server_get_screen_size(toplevel->server, &screen_w, &screen_h))
        return false;

    return geo.width >= (screen_w * 4 / 5);
}

/* ------------------------------------------------------------------ */
/* Positioning                                                        */
/* ------------------------------------------------------------------ */

void position_taskbar(struct fg_toplevel *toplevel) {
    struct fg_server *server = toplevel->server;

    server->taskbar = toplevel;

    int screen_w, screen_h;
    if (!server_get_screen_size(server, &screen_w, &screen_h)) return;

    struct wlr_box geo;
    fg_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
    int bar_h = geo.height > 0 ? geo.height : 30;

    int target_y = screen_h - bar_h;

    wlr_scene_node_set_position(&toplevel->scene_tree->node,
        0, target_y);

    /* Hint Wine to use the full screen width */
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, screen_w, bar_h);

    /*
     * DO NOT raise_to_top here.  This function is called on every
     * taskbar commit (clock tick, redraw, etc.).  Raising here buries
     * the focused window under the taskbar — the z-order inversion bug.
     * The taskbar is raised once on its initial map in xdg_toplevel_map.
     */
}

int get_taskbar_height(struct fg_server *server) {
    if (!server->taskbar) return 0;
    if (!server->taskbar->xdg_toplevel->base->surface->mapped) return 0;

    struct wlr_box geo;
    fg_xdg_surface_get_geometry(server->taskbar->xdg_toplevel->base, &geo);
    return geo.height > 0 ? geo.height : 30;
}

/* ------------------------------------------------------------------ */
/* Deferred detection from commit handler                             */
/* ------------------------------------------------------------------ */

void try_detect_taskbar(struct fg_toplevel *toplevel) {
    struct fg_server *server = toplevel->server;

    /*
     * Validate existing taskbar — if it's gone stale (unmapped,
     * destroyed, or otherwise invalid), clear it so we can recover.
     */
    if (server->taskbar) {
        struct fg_toplevel *tb = server->taskbar;
        if (!tb->xdg_toplevel ||
            !tb->xdg_toplevel->base ||
            !tb->xdg_toplevel->base->surface ||
            !tb->xdg_toplevel->base->surface->mapped) {
            wlr_log(WLR_INFO,
                "Stale taskbar pointer detected, clearing for re-scan");
            server->taskbar = NULL;
        }
    }

    /* Current taskbar is still valid and mapped — nothing to do */
    if (server->taskbar) return;

    if (!toplevel->xdg_toplevel->base->surface->mapped) return;

    if (is_taskbar(toplevel)) {
        wlr_log(WLR_INFO,
            "Taskbar detected on commit: app_id=%s geo=%dx%d",
            toplevel->xdg_toplevel->app_id ?: "(null)",
            toplevel->xdg_toplevel->base->geometry.width,
            toplevel->xdg_toplevel->base->geometry.height);

        position_taskbar(toplevel);
        wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    }
}

/* ------------------------------------------------------------------ */
/* Full re-scan — called after taskbar loss (destroy/unmap)           */
/* ------------------------------------------------------------------ */

void taskbar_rescan_all(struct fg_server *server) {
    if (server->taskbar) return;   /* already recovered */

    struct fg_toplevel *tl;
    wl_list_for_each(tl, &server->toplevels, link) {
        if (!tl->xdg_toplevel ||
            !tl->xdg_toplevel->base ||
            !tl->xdg_toplevel->base->surface ||
            !tl->xdg_toplevel->base->surface->mapped)
            continue;

        if (is_taskbar(tl)) {
            wlr_log(WLR_INFO,
                "Taskbar re-acquired via rescan: app_id=%s",
                tl->xdg_toplevel->app_id ?: "(null)");
            position_taskbar(tl);
            wlr_scene_node_raise_to_top(&tl->scene_tree->node);
            return;
        }
    }

    wlr_log(WLR_INFO,
        "Taskbar rescan found no candidate — respawning explorer");

    respawn_wine_explorer(server);
}
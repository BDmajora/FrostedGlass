/*
 * fg_desktop.c — Desktop root window detection and bottom-pinning.
 *
 * The YetiOS shell launches desktop.exe, a full-screen Wine window that
 * paints the wallpaper.  This module finds that window and pins it to the
 * BOTTOM of the scene graph, beneath the taskbar and every application
 * window.
 *
 * Why a Wine window instead of a compositor rect:
 *   The old code painted a solid background_rect in the scene graph.  But
 *   a rect is not a Wine surface — over it, Wine never holds pointer focus
 *   (see wayland_pointer.c: `if (pointer->focused_hwnd == hwnd)`), so Wine
 *   never issues wl_pointer_set_cursor and the bare wlroots cursor shows
 *   through.  With desktop.exe covering the screen, the pointer is ALWAYS
 *   over a Wine surface, so Wine always sets its own cursor.
 *
 * Detection mirrors fg_taskbar.c.  desktop.exe is its own process, so its
 * app_id is "desktop.exe" — distinct from the taskbar, which shares
 * explorer.exe.  We match on app_id, falling back to the window title
 * ("YetiOS Desktop") in case Wine's app_id derivation surprises us (it has
 * before — that's why the taskbar detection is geometry-based).
 */

#define _POSIX_C_SOURCE 200809L

#include <string.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "fg_desktop.h"
#include "fg_output.h"

/*
 * Contract with desktop.exe (see desktop_defs.h in the desktop program):
 *   DESKTOP_WND_CLASS = "YetiOSDesktop"
 *   DESKTOP_WND_TITLE = "YetiOS Desktop"
 *
 * Wine reports the *process* name as app_id, so we expect "desktop.exe".
 * The title is set from DESKTOP_WND_TITLE and is the reliable fallback.
 */
#define FG_DESKTOP_TITLE  "YetiOS Desktop"

/* ------------------------------------------------------------------ */
/* Detection                                                          */
/* ------------------------------------------------------------------ */

bool is_desktop(struct fg_toplevel *toplevel) {
    /* Already tagged — fast path */
    if (toplevel->server->desktop == toplevel) return true;

    struct wlr_xdg_toplevel *xt = toplevel->xdg_toplevel;
    if (!xt) return false;

    const char *app_id = xt->app_id;
    const char *title = xt->title;

    /*
     * Primary match: app_id is the process name.  Wine may vary the
     * case, so check the common forms defensively (same approach the
     * taskbar uses for explorer.exe).
     */
    if (app_id &&
        (strcmp(app_id, "desktop.exe") == 0 ||
         strcmp(app_id, "Desktop.exe") == 0 ||
         strcmp(app_id, "DESKTOP.EXE") == 0)) {
        return true;
    }

    /*
     * Fallback match: the window title.  desktop.exe sets a fixed,
     * unique title, so this is a safe secondary signal if the app_id
     * isn't what we expect.
     */
    if (title && strcmp(title, FG_DESKTOP_TITLE) == 0) {
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* Positioning                                                        */
/* ------------------------------------------------------------------ */

void position_desktop(struct fg_toplevel *toplevel) {
    struct fg_server *server = toplevel->server;

    server->desktop = toplevel;

    int screen_w, screen_h;
    if (!server_get_screen_size(server, &screen_w, &screen_h)) return;

    /* Cover the entire screen, top-left origin. */
    wlr_scene_node_set_position(&toplevel->scene_tree->node, 0, 0);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, screen_w, screen_h);

    /*
     * Pin to the very bottom of the scene graph — the inverse of the
     * taskbar.  Every other window (taskbar, apps) sits on top.
     *
     * Safe to call on every commit: lower_to_bottom is idempotent, and
     * we never raise the desktop, so it can't bury other windows the way
     * an over-eager taskbar raise would.
     */
    wlr_scene_node_lower_to_bottom(&toplevel->scene_tree->node);
}

/* ------------------------------------------------------------------ */
/* Deferred detection from commit handler                             */
/* ------------------------------------------------------------------ */

void try_detect_desktop(struct fg_toplevel *toplevel) {
    struct fg_server *server = toplevel->server;

    /*
     * Validate existing desktop — if it's gone stale (unmapped,
     * destroyed, or otherwise invalid), clear it so we can recover.
     */
    if (server->desktop) {
        struct fg_toplevel *d = server->desktop;
        if (!d->xdg_toplevel ||
            !d->xdg_toplevel->base ||
            !d->xdg_toplevel->base->surface ||
            !d->xdg_toplevel->base->surface->mapped) {
            wlr_log(WLR_INFO,
                "Stale desktop pointer detected, clearing for re-scan");
            server->desktop = NULL;
        }
    }

    /* Current desktop is still valid and mapped — nothing to do */
    if (server->desktop) return;

    if (!toplevel->xdg_toplevel->base->surface->mapped) return;

    if (is_desktop(toplevel)) {
        wlr_log(WLR_INFO,
            "Desktop detected on commit: app_id=%s title=\"%s\"",
            toplevel->xdg_toplevel->app_id ?: "(null)",
            toplevel->xdg_toplevel->title ?: "");

        position_desktop(toplevel);
    }
}

/* ------------------------------------------------------------------ */
/* Full re-scan — called after desktop loss (destroy/unmap)           */
/* ------------------------------------------------------------------ */

void desktop_rescan_all(struct fg_server *server) {
    if (server->desktop) return;   /* already recovered */

    struct fg_toplevel *tl;
    wl_list_for_each(tl, &server->toplevels, link) {
        if (!tl->xdg_toplevel ||
            !tl->xdg_toplevel->base ||
            !tl->xdg_toplevel->base->surface ||
            !tl->xdg_toplevel->base->surface->mapped)
            continue;

        if (is_desktop(tl)) {
            wlr_log(WLR_INFO,
                "Desktop re-acquired via rescan: app_id=%s",
                tl->xdg_toplevel->app_id ?: "(null)");
            position_desktop(tl);
            return;
        }
    }

    wlr_log(WLR_INFO,
        "Desktop rescan found no candidate — "
        "will retry on next map/commit");
}
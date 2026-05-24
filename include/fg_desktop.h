/*
 * fg_desktop.h — Desktop root window detection and bottom-pinning.
 *
 * The desktop is a full-screen Wine window (desktop.exe) that paints the
 * wallpaper.  Unlike the old compositor-painted background rect, it is a
 * real Wine surface, so the pointer is always over a Wine window and Wine
 * always sets its own cursor — the bare wlroots cursor is never shown.
 *
 * This module mirrors fg_taskbar.c: detect by app_id/title, then pin to
 * the BOTTOM of the scene graph (the inverse of the taskbar, which is
 * raised to the top).
 */

#ifndef FG_DESKTOP_H
#define FG_DESKTOP_H

#include "fg_types.h"

/*
 * Heuristic: is this toplevel the desktop.exe wallpaper window?
 *
 * desktop.exe is its own process, so Wine's app_id is "desktop.exe"
 * (unlike the taskbar, which shares explorer.exe with other windows).
 * We match on app_id primarily, with the window title as a fallback.
 */
bool is_desktop(struct fg_toplevel *toplevel);

/*
 * Pin the desktop to the bottom of the scene graph and size it to cover
 * the whole screen.  Sets position, size, and z-order (lower-to-bottom).
 * Safe to call on every commit (idempotent).
 */
void position_desktop(struct fg_toplevel *toplevel);

/*
 * Called from the commit handler.  If we haven't identified the desktop
 * yet and this toplevel now looks like the desktop, tag and pin it.
 */
void try_detect_desktop(struct fg_toplevel *toplevel);

/*
 * Scan all mapped toplevels looking for the desktop.
 * Called after desktop loss (destroy/unmap) to recover immediately.
 */
void desktop_rescan_all(struct fg_server *server);

#endif /* FG_DESKTOP_H */
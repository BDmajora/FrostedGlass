/*
 * fg_taskbar.h — Taskbar detection and positioning for frostedglass.
 */

#ifndef FG_TASKBAR_H
#define FG_TASKBAR_H

#include "fg_types.h"

/*
 * Heuristic: is this toplevel Wine's taskbar?
 *
 * Wine's Wayland driver sets app_id to the process name ("explorer.exe"),
 * NOT the Win32 window class ("Shell_TrayWnd").  So we detect the taskbar
 * by geometry: full screen width + short height + from explorer.exe.
 */
bool is_taskbar(struct fg_toplevel *toplevel);

/*
 * Pin the taskbar to the bottom of the screen.
 * Sets position only — does NOT touch z-order.
 * Safe to call on every commit (idempotent).
 */
void position_taskbar(struct fg_toplevel *toplevel);

/*
 * Get the taskbar height, or 0 if no taskbar is mapped.
 */
int get_taskbar_height(struct fg_server *server);

/*
 * Called from commit handler.  If we haven't identified the taskbar
 * yet and this toplevel's geometry now looks like a taskbar, tag it.
 */
void try_detect_taskbar(struct fg_toplevel *toplevel);

#endif /* FG_TASKBAR_H */
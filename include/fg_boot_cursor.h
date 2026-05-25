/*
 * fg_boot_cursor.h — Embedded Win32-style boot cursor.
 *
 * Before Wine has ever issued a wl_pointer.set_cursor, the compositor has
 * no authentic Win32 cursor to show.  The old code filled that gap with
 * the wlroots xcursor "default", which flashes a Linux pointer for the
 * first few hundred milliseconds — breaking the illusion of a Windows
 * shell.
 *
 * This module builds a compositor-OWNED wlr_buffer containing a classic
 * Windows arrow, drawn from a hard-coded bitmap.  It depends on nothing
 * external (no theme, no Wine, no mouse motion), so it can be the cursor
 * from the very first frame.  Once Wine sets its real arrow, the normal
 * capture path in fg_input.c replaces this buffer seamlessly.
 */

#ifndef FG_BOOT_CURSOR_H
#define FG_BOOT_CURSOR_H

struct wlr_buffer;

/*
 * Create a freshly-allocated wlr_buffer holding the Win32 arrow.
 *
 * The returned buffer has NO locks held — the caller takes ownership and
 * must either lock it (wlr_buffer_lock) to keep it, or drop it
 * (wlr_buffer_drop) once other locks exist.  The typical pattern:
 *
 *     struct wlr_buffer *b = fg_boot_cursor_create(&hx, &hy);
 *     wlr_cursor_set_buffer(cursor, b, hx, hy, 1.0f);  // takes a lock
 *     server->system_cursor_buffer = wlr_buffer_lock(b); // our lock
 *     wlr_buffer_drop(b);                                // release creator ref
 *
 * *hotspot_x / *hotspot_y are filled with the arrow tip (0,0 for an arrow).
 * Returns NULL on allocation failure.
 */
struct wlr_buffer *fg_boot_cursor_create(int *hotspot_x, int *hotspot_y);

#endif /* FG_BOOT_CURSOR_H */
/*
 * fg_toplevel.h — XDG toplevel, popup, and focus management.
 */

#ifndef FG_TOPLEVEL_H
#define FG_TOPLEVEL_H

#include "fg_types.h"

/*
 * Focus the given toplevel: raise it, activate it, and send
 * keyboard enter.  Safe to call with NULL (no-op).
 */
void focus_toplevel(struct fg_toplevel *toplevel);

/*
 * Hit-test the scene graph at layout coordinates (lx, ly).
 * Returns the fg_toplevel under the pointer (or NULL), and
 * fills *surface / *sx / *sy with the surface-local hit.
 */
struct fg_toplevel *toplevel_at(struct fg_server *server,
    double lx, double ly, struct wlr_surface **surface,
    double *sx, double *sy);

/*
 * Listener callback for xdg_shell new_toplevel events.
 * Allocates an fg_toplevel wrapper and wires all per-window listeners.
 */
void server_new_xdg_toplevel(struct wl_listener *listener, void *data);

/*
 * Listener callback for xdg_shell new_popup events.
 * Allocates an fg_popup wrapper and wires commit/destroy listeners.
 */
void server_new_xdg_popup(struct wl_listener *listener, void *data);

#endif /* FG_TOPLEVEL_H */

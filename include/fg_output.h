/*
 * fg_output.h — Output (display/monitor) management for frostedglass.
 */

#ifndef FG_OUTPUT_H
#define FG_OUTPUT_H

#include "fg_types.h"

/*
 * Listener callback for backend new_output events.
 * Initializes rendering, picks preferred mode, creates scene output,
 * and wires up per-output listeners (frame, request_state, destroy).
 */
void server_new_output(struct wl_listener *listener, void *data);

/*
 * Query the usable screen dimensions from the first output.
 * Returns false if no outputs are available yet.
 */
bool server_get_screen_size(struct fg_server *server, int *w, int *h);

/*
 * (Re-)create or resize the background rect to cover all outputs.
 * Called from output setup and whenever the output layout changes.
 */
void server_update_background(struct fg_server *server);

/*
 * Initialise wlr-output-management-v1 so that wlr-randr (and desk.cpl)
 * can enumerate outputs / modes and apply resolution / refresh / VRR
 * changes at runtime.  Call once from server_init().
 */
void server_init_output_management(struct fg_server *server);

/*
 * Push the current output configuration to all bound
 * wlr-output-management-v1 clients (e.g. wlr-randr).
 * Called after any mode or layout change.
 */
void server_send_output_config(struct fg_server *server);

#endif /* FG_OUTPUT_H */
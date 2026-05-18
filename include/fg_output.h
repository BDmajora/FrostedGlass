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

#endif /* FG_OUTPUT_H */

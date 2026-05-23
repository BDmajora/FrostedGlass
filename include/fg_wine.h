/*
 * fg_wine.h — Wine process launcher and lifecycle management.
 */

#ifndef FG_WINE_H
#define FG_WINE_H

#include "fg_types.h"

/*
 * Fork and exec Wine explorer.exe as the desktop shell.
 * Applies registry preferences from ~/.frostedglass_prefs.reg first.
 * The child process is tracked via server->wine_pid.
 */
void launch_wine(struct fg_server *server, const char *socket);

/*
 * Install a SIGCHLD handler that reaps Wine and terminates the
 * event loop when Wine exits.  Must be called once before the
 * event loop starts.  Stores a reference to `server` in a file-
 * scoped global (unavoidable with POSIX signal handlers).
 */
void wine_install_sigchld(struct fg_server *server);

#endif /* FG_WINE_H */
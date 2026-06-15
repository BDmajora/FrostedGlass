/*
 * fg_audio.c — PipeWire audio stack lifecycle management.
 *
 * Mirrors fg_wine.c's process-handling conventions: fork/execlp with
 * output redirected to a /tmp log, pids tracked on the server struct,
 * reaping left to the shared SIGCHLD handler installed by
 * wine_install_sigchld().
 *
 * Startup order matters:
 *   1. pipewire        — wait until $XDG_RUNTIME_DIR/pipewire-0 exists
 *   2. wireplumber     — connects to that socket, builds the session
 *
 * launch_wine() must run after this returns, so that when Moonshine's
 * mmdevapi walks its driver list ("pipewire,alsa"), winepipewire.drv's
 * pw_context_connect() finds a live daemon and wins with
 * Priority_Preferred.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wlr/util/log.h>

#include "fg_audio.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void pipewire_socket_path(char *buf, size_t size) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    snprintf(buf, size, "%s/pipewire-0", xdg ? xdg : "/tmp");
}

static pid_t spawn_daemon(const char *binary, const char *log_path) {
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log(WLR_ERROR, "fork() failed for %s", binary);
        return -1;
    }
    if (pid == 0) {
        int log_fd = open(log_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        execlp(binary, binary, NULL);
        _exit(127);
    }
    return pid;
}

/* Wait up to ~3s for a path to appear. Returns true if it did. */
static bool wait_for_path(const char *path) {
    const struct timespec step = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    for (int i = 0; i < 60; i++) {
        if (access(path, F_OK) == 0) return true;
        nanosleep(&step, NULL);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void launch_audio(struct fg_server *server) {
    char socket_path[512];
    pipewire_socket_path(socket_path, sizeof(socket_path));

    /*
     * If a socket already exists, an external session (e.g. running
     * frostedglass nested for development) already provides audio.
     * Don't double-spawn daemons into it.
     */
    if (access(socket_path, F_OK) == 0) {
        wlr_log(WLR_INFO,
            "PipeWire socket already present at %s — "
            "using existing audio session", socket_path);
        return;
    }

    server->pipewire_pid = spawn_daemon("pipewire", "/tmp/pipewire.log");
    if (server->pipewire_pid <= 0) {
        wlr_log(WLR_ERROR,
            "Failed to spawn pipewire — Wine audio will fall back to ALSA");
        return;
    }
    wlr_log(WLR_INFO, "pipewire launched (pid %d)", server->pipewire_pid);

    /*
     * Wait for the client socket before starting the session manager
     * (and, later, Wine).  wireplumber would retry on its own, but
     * Wine's driver probe happens exactly once per mmdevapi load — the
     * socket must exist before explorer.exe starts.
     */
    if (!wait_for_path(socket_path)) {
        wlr_log(WLR_ERROR,
            "PipeWire socket %s did not appear within 3s — "
            "Wine audio will fall back to ALSA (see /tmp/pipewire.log)",
            socket_path);
        return;
    }
    wlr_log(WLR_INFO, "PipeWire socket ready at %s", socket_path);

    server->wireplumber_pid =
        spawn_daemon("wireplumber", "/tmp/wireplumber.log");
    if (server->wireplumber_pid <= 0) {
        wlr_log(WLR_ERROR,
            "Failed to spawn wireplumber — devices will not be linked "
            "and no default endpoint will exist");
        return;
    }
    wlr_log(WLR_INFO, "wireplumber launched (pid %d)",
        server->wireplumber_pid);

    /* pipewire-pulse: the PulseAudio-protocol shim. Native PipeWire clients
     * (Moonshine via winepipewire.drv) bypass it entirely, but pulse-API apps
     * — Chrome, Firefox, anything on libpulse/cubeb — connect through it. With
     * it absent they get "PulseAudio: Connection refused", and the ALSA
     * `default` fallback is itself the pulse plugin, so that fails too.
     * Spawn last: it connects to the running pipewire daemon. Non-fatal —
     * native-PipeWire audio (the Win32 surfaces) is unaffected if it fails. */
    server->pipewire_pulse_pid =
        spawn_daemon("pipewire-pulse", "/tmp/pipewire-pulse.log");
    if (server->pipewire_pulse_pid <= 0) {
        wlr_log(WLR_ERROR,
            "Failed to spawn pipewire-pulse — pulse-API apps "
            "(browsers, etc.) will have no audio");
    } else {
        wlr_log(WLR_INFO, "pipewire-pulse launched (pid %d)",
            server->pipewire_pulse_pid);
    }
}

void shutdown_audio(struct fg_server *server) {
    /* Tear down in reverse dependency order: pulse shim first (it rides on
     * pipewire), then wireplumber, then pipewire itself. */
    if (server->pipewire_pulse_pid > 0) {
        kill(server->pipewire_pulse_pid, SIGTERM);
        server->pipewire_pulse_pid = 0;
    }
    if (server->wireplumber_pid > 0) {
        kill(server->wireplumber_pid, SIGTERM);
        server->wireplumber_pid = 0;
    }
    if (server->pipewire_pid > 0) {
        kill(server->pipewire_pid, SIGTERM);
        server->pipewire_pid = 0;
    }
    /* Reaping is handled by the shared SIGCHLD handler. */
}
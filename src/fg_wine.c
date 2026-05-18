/*
 * fg_wine.c — Wine process launcher and SIGCHLD reaping.
 *
 * Spawns Wine explorer.exe as the desktop shell, optionally applying
 * a user registry file first.  A SIGCHLD handler reaps the Wine
 * process so the compositor can shut down cleanly when Wine exits.
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <wlr/util/log.h>

#include "fg_wine.h"

/* ------------------------------------------------------------------ */
/* SIGCHLD — reap Wine and terminate event loop                       */
/* ------------------------------------------------------------------ */

static struct fg_server *g_server = NULL;

static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (g_server && pid == g_server->wine_pid) {
            wlr_log(WLR_INFO, "Wine exited (status %d), shutting down",
                WEXITSTATUS(status));
            if (g_server->display) {
                wl_display_terminate(g_server->display);
            }
        }
    }
}

void wine_install_sigchld(struct fg_server *server) {
    g_server = server;

    struct sigaction sa = { .sa_handler = sigchld_handler };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* Registry preferences                                               */
/* ------------------------------------------------------------------ */

static void apply_registry_prefs(void) {
    const char *home = getenv("HOME");
    if (!home) return;

    char reg_path[512];
    snprintf(reg_path, sizeof(reg_path),
        "%s/.frostedglass_prefs.reg", home);

    /* Fall back to legacy name */
    if (access(reg_path, R_OK) != 0) {
        snprintf(reg_path, sizeof(reg_path),
            "%s/.wslk_prefs.reg", home);
    }

    if (access(reg_path, R_OK) != 0) return;

    pid_t reg_pid = fork();
    if (reg_pid == 0) {
        execlp("wine", "wine", "regedit", "/s", reg_path, NULL);
        _exit(127);
    }
    if (reg_pid > 0) {
        waitpid(reg_pid, NULL, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Wine launcher                                                      */
/* ------------------------------------------------------------------ */

void launch_wine(struct fg_server *server, const char *socket) {
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log(WLR_ERROR, "fork() failed for Wine");
        return;
    }
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", socket, 1);
        unsetenv("DISPLAY");
        setenv("WINEWAYLAND", "1", 1);

        apply_registry_prefs();

        const char *res = server->wine_desktop_res;
        if (res) {
            char desktop_arg[128];
            snprintf(desktop_arg, sizeof(desktop_arg),
                "/desktop=shell,%s", res);
            execlp("wine", "wine", "explorer", desktop_arg, NULL);
        } else {
            execlp("wine", "wine", "explorer", "/desktop=shell", NULL);
        }
        _exit(127);
    }
    server->wine_pid = pid;
    wlr_log(WLR_INFO, "Wine explorer.exe launched (pid %d)", pid);
}

/*
 * fg_wine.c — Wine process launcher and SIGCHLD reaping.
 *
 * Spawns Wine explorer.exe as the desktop shell, optionally applying
 * a user registry file first.  A SIGCHLD handler reaps the Wine
 * process so the compositor can shut down cleanly when Wine exits.
 */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
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
        /*
         * Just reap zombies.  We used to terminate the compositor when
         * wine_pid exited, but Wine's process tree is complex:
         *
         *   1. Our fork() child calls execlp("wine", "wine", "explorer"...)
         *   2. The "wine" launcher may fork internally and exit, so
         *      wine_pid dies almost immediately — that doesn't mean
         *      the desktop session is over.
         *   3. Closing any Win32 app causes Wine subprocesses to exit,
         *      triggering SIGCHLD — we must not shut down the compositor
         *      just because a child exited.
         *
         * Instead, the compositor runs until Ctrl+Alt+Backspace or until
         * wl_display_terminate() is called from elsewhere.
         */
        if (g_server && pid == g_server->wine_pid) {
            wlr_log(WLR_INFO,
                "Wine launcher process exited (status %d) — "
                "this is normal, Wine services continue running",
                WEXITSTATUS(status));
        }
        if (g_server && pid == g_server->pipewire_pid) {
            wlr_log(WLR_ERROR,
                "pipewire exited (status %d) — Wine audio is dead; "
                "see /tmp/pipewire.log", WEXITSTATUS(status));
            g_server->pipewire_pid = 0;
        }
        if (g_server && pid == g_server->wireplumber_pid) {
            wlr_log(WLR_ERROR,
                "wireplumber exited (status %d) — no session manager; "
                "see /tmp/wireplumber.log", WEXITSTATUS(status));
            g_server->wireplumber_pid = 0;
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

    if (access(reg_path, R_OK) != 0) {
        wlr_log(WLR_INFO, "No registry prefs file found, skipping");
        return;
    }

    wlr_log(WLR_INFO, "Applying Wine registry prefs from %s", reg_path);

    pid_t reg_pid = fork();
    if (reg_pid == 0) {
        execlp("wine", "wine", "regedit", "/s", reg_path, NULL);
        _exit(127);
    }
    if (reg_pid > 0) {
        int status;
        waitpid(reg_pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            wlr_log(WLR_INFO, "Registry prefs applied successfully");
        } else {
            wlr_log(WLR_ERROR, "Registry import exited with status %d",
                WEXITSTATUS(status));
        }
    }

    /*
     * Kill wineserver so it flushes all registry changes to disk.
     * --kill sends SIGKILL to the server, which forces an immediate
     * exit and disk flush.  --wait would be gentler but can hang
     * indefinitely if the server's persistence timeout hasn't expired.
     * The next Wine process (explorer.exe) will start a fresh
     * wineserver that reads the now-updated registry files from disk.
     */
    wlr_log(WLR_INFO, "Killing wineserver to flush registry ...");
    pid_t ws_pid = fork();
    if (ws_pid == 0) {
        execlp("wineserver", "wineserver", "--kill", NULL);
        _exit(127);
    }
    if (ws_pid > 0) {
        int status;
        waitpid(ws_pid, &status, 0);
        wlr_log(WLR_INFO, "wineserver killed, registry flushed to disk");
    }
}

/* ------------------------------------------------------------------ */
/* Wine prefix initialization check                                   */
/* ------------------------------------------------------------------ */

/*
 * If the Wine prefix doesn't exist yet, run wineboot --init to create
 * it. This is separate from the registry import because wineboot needs
 * to finish before we can import registry files.
 */
static void ensure_wine_prefix(void) {
    const char *home = getenv("HOME");
    if (!home) return;

    char prefix_check[512];
    snprintf(prefix_check, sizeof(prefix_check),
        "%s/.wine/system.reg", home);

    if (access(prefix_check, F_OK) == 0) {
        wlr_log(WLR_INFO, "Wine prefix exists, skipping wineboot --init");
        return;
    }

    wlr_log(WLR_INFO, "Wine prefix not found, running wineboot --init ...");

    pid_t init_pid = fork();
    if (init_pid == 0) {
        execlp("wineboot", "wineboot", "--init", NULL);
        _exit(127);
    }
    if (init_pid > 0) {
        int status;
        waitpid(init_pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            wlr_log(WLR_INFO, "Wine prefix initialized");
        } else {
            wlr_log(WLR_ERROR, "wineboot --init exited with status %d",
                WEXITSTATUS(status));
        }
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

        /*
         * ---- DIAGNOSTIC LOGGING ----------------------------------------
         *
         * WAYLAND_DEBUG=1  — Wine's Wayland driver logs every protocol
         *   request and event to stderr.  When the compositor posts a fatal
         *   protocol error the last few lines name the exact object and call
         *   that triggered it.
         *
         * WINEDEBUG=+waylanddrv  — turns on the Wine-side ERR/TRACE channel
         *   for the Wayland driver, so we see the driver's own log lines
         *   (including "Failed to read events from the compositor") mixed
         *   in with the raw protocol stream.
         *
         * All output from this child (and every Wine process it spawns) goes
         * to /tmp/wine_wayland_debug.log.  Read it with:
         *   tail -f /tmp/wine_wayland_debug.log
         * After reproducing the crash look for the last few lines — the
         * protocol error line and the wl_display::error event are right
         * before the connection drops.
         * ---------------------------------------------------------------- */
        setenv("WAYLAND_DEBUG", "1", 1);
        setenv("WINEDEBUG", "+waylanddrv", 1);

        /* Redirect this child's stdout + stderr to the log file.
         * O_CREAT|O_WRONLY|O_TRUNC — fresh file on every launch. */
        int log_fd = open("/tmp/wine_wayland_debug.log",
                          O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        /* Ensure prefix exists before doing anything else */
        ensure_wine_prefix();

        /* Apply registry prefs (taskbar position, DPI, etc.) */
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
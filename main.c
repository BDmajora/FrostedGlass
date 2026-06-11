/*
 * main.c — frostedglass compositor entry point.
 *
 * Minimal wlroots compositor for YetiOS.
 *
 * Cursor strategy: NO xcursor theme.  NO boot cursor.  The compositor
 * starts with no visible cursor.  Wine boots, sends the Win32 arrow
 * via yetios_cursor_manager_v1, and that becomes THE cursor forever.
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "fg_types.h"
#include "fg_output.h"
#include "fg_toplevel.h"
#include "fg_input.h"
#include "fg_wine.h"
#include "fg_audio.h"
#include "fg_cursor_override.h"

/* ------------------------------------------------------------------ */
/* Argument parsing                                                   */
/* ------------------------------------------------------------------ */

static const char *parse_args(int argc, char *argv[]) {
    const char *resolution = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--res") == 0 && i + 1 < argc) {
            resolution = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage: frostedglass [--res WxH]\n"
                "\n"
                "Minimal Wayland compositor for YetiOS.\n"
                "Launches Wine explorer.exe as the desktop shell.\n"
                "\n"
                "Options:\n"
                "  --res WxH   Wine desktop resolution (e.g. 1920x1080).\n"
                "              Omit for automatic (uses output native res).\n"
                "\n"
                "Compositor keybindings:\n"
                "  Ctrl+Alt+Backspace   Kill compositor (emergency exit)\n");
            exit(0);
        }
    }
    return resolution;
}

/* ------------------------------------------------------------------ */
/* Global filter — hide protocols we don't want Wine to use           */
/* ------------------------------------------------------------------ */

static bool global_filter(const struct wl_client *client,
    const struct wl_global *global, void *data) {
    (void)client;
    (void)data;

    /* Block wp_cursor_shape_manager_v1 so Wine falls back to uploading
     * its own cursor bitmap via wl_pointer_set_cursor. */
    const char *iface = wl_global_get_interface(global)->name;
    if (strcmp(iface, "wp_cursor_shape_manager_v1") == 0)
        return false;

    return true;
}

/* ------------------------------------------------------------------ */
/* Server initialization                                              */
/* ------------------------------------------------------------------ */

static bool server_init(struct fg_server *server) {
    server->display = wl_display_create();
    assert(server->display);

    wl_display_set_global_filter(server->display, global_filter, NULL);

    server->backend = wlr_backend_autocreate(
        wl_display_get_event_loop(server->display), NULL);
    assert(server->backend);

    server->renderer = wlr_renderer_autocreate(server->backend);
    assert(server->renderer);
    wlr_renderer_init_wl_display(server->renderer, server->display);

    server->allocator =
        wlr_allocator_autocreate(server->backend, server->renderer);
    assert(server->allocator);

    wlr_compositor_create(server->display, 6, server->renderer);
    wlr_subcompositor_create(server->display);
    wlr_viewporter_create(server->display);
    wlr_data_device_manager_create(server->display);

    /* Outputs */
    server->output_layout = wlr_output_layout_create(server->display);
    wl_list_init(&server->outputs);
    server->new_output.notify = server_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);

    /* Output management — enables wlr-randr / desk.cpl mode control */
    server_init_output_management(server);

    /* Scene graph */
    server->scene = wlr_scene_create();
    server->scene_layout =
        wlr_scene_attach_output_layout(server->scene, server->output_layout);

    /* XDG shell */
    wl_list_init(&server->toplevels);
    server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
    server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel,
        &server->new_xdg_toplevel);
    server->new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup,
        &server->new_xdg_popup);

    /* Cursor — create the wlr_cursor but do NOT set any image.
     * The cursor will be invisible until Wine sends one.
     * xcursor_manager is kept only as emergency fallback. */
    server->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

    server->cursor_motion.notify = cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
    server->cursor_motion_absolute.notify = cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute,
        &server->cursor_motion_absolute);
    server->cursor_button.notify = cursor_button;
    wl_signal_add(&server->cursor->events.button, &server->cursor_button);
    server->cursor_axis.notify = cursor_axis;
    wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
    server->cursor_frame.notify = cursor_frame;
    wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

    /* Input / seat */
    wl_list_init(&server->keyboards);
    server->new_input.notify = server_new_input;
    wl_signal_add(&server->backend->events.new_input, &server->new_input);

    server->seat = wlr_seat_create(server->display, "seat0");
    server->request_set_cursor.notify = seat_request_cursor;
    wl_signal_add(&server->seat->events.request_set_cursor,
        &server->request_set_cursor);
    server->request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server->seat->events.request_set_selection,
        &server->request_set_selection);

    /* Win32 cursor protocol — creates yetios_cursor_manager_v1 global.
     * Wine will bind this and send the cursor at boot. */
    server->cursor_override = cursor_override_init(server);
    if (!server->cursor_override) {
        wlr_log(WLR_ERROR,
            "Failed to init cursor override protocol");
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Server teardown                                                    */
/* ------------------------------------------------------------------ */

static void server_finish(struct fg_server *server) {
    if (server->wine_pid > 0) {
        kill(server->wine_pid, SIGTERM);
        waitpid(server->wine_pid, NULL, 0);
    }

    shutdown_audio(server);

    wl_display_destroy_clients(server->display);

    cursor_override_destroy(server->cursor_override);

    if (server->system_cursor_buffer) {
        wlr_buffer_unlock(server->system_cursor_buffer);
        server->system_cursor_buffer = NULL;
    }

    wlr_scene_node_destroy(&server->scene->tree.node);
    wlr_xcursor_manager_destroy(server->cursor_mgr);
    wlr_cursor_destroy(server->cursor);
    wlr_output_layout_destroy(server->output_layout);
    wlr_allocator_destroy(server->allocator);
    wlr_renderer_destroy(server->renderer);
    wlr_backend_destroy(server->backend);
    wl_display_destroy(server->display);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    wlr_log_init(WLR_DEBUG, NULL);

    int log_fd = open("/tmp/frostedglass.log",
                      O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (log_fd >= 0) {
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
    }

    const char *resolution = parse_args(argc, argv);

    struct fg_server server = {0};
    server.wine_desktop_res = resolution;

    wine_install_sigchld(&server);
    server_init(&server);

    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_log(WLR_ERROR, "Failed to open Wayland socket");
        wlr_backend_destroy(server.backend);
        return 1;
    }

    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "Failed to start backend");
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.display);
        return 1;
    }

    wlr_log(WLR_INFO, "frostedglass running on %s — "
        "waiting for Wine to send cursor...", socket);

    setenv("WAYLAND_DISPLAY", socket, 1);

    /* Auto-detect resolution from first output */
    static char auto_res[32];
    if (!server.wine_desktop_res && !wl_list_empty(&server.outputs)) {
        struct fg_output *first_output =
            wl_container_of(server.outputs.next, first_output, link);
        struct wlr_output *out = first_output->wlr_output;
        if (out->width > 0 && out->height > 0) {
            snprintf(auto_res, sizeof(auto_res), "%dx%d",
                out->width, out->height);
            server.wine_desktop_res = auto_res;
            wlr_log(WLR_INFO, "Auto-detected resolution: %s", auto_res);
        }
    }

    /* Audio stack must be up before Wine probes its audio drivers */
    launch_audio(&server);

    launch_wine(&server, socket);

    wl_display_run(server.display);

    server_finish(&server);

    wlr_log(WLR_INFO, "frostedglass shut down cleanly");
    return 0;
}
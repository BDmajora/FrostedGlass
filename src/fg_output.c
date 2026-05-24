/*
 * fg_output.c — Output (display/monitor) management for frostedglass.
 *
 * Handles new outputs from the backend, per-frame rendering via
 * the wlroots scene graph, mode/state negotiation, and teardown.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "fg_output.h"

/* ------------------------------------------------------------------ */
/* Per-output listener callbacks                                      */
/* ------------------------------------------------------------------ */

static void output_frame(struct wl_listener *listener, void *data) {
    struct fg_output *output = wl_container_of(listener, output, frame);
    struct wlr_scene *scene = output->server->scene;

    struct wlr_scene_output *scene_output =
        wlr_scene_get_scene_output(scene, output->wlr_output);
    wlr_scene_output_commit(scene_output, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
    struct fg_output *output =
        wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    struct fg_output *output = wl_container_of(listener, output, destroy);
    struct fg_server *server = output->server;
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);

    /* Resize background to remaining outputs */
    server_update_background(server);
}

/* ------------------------------------------------------------------ */
/* Screen dimension helpers                                           */
/* ------------------------------------------------------------------ */

bool server_get_screen_size(struct fg_server *server, int *w, int *h) {
    if (wl_list_empty(&server->outputs)) return false;

    struct fg_output *first =
        wl_container_of(server->outputs.next, first, link);
    struct wlr_output *out = first->wlr_output;
    if (out->width <= 0 || out->height <= 0) return false;

    *w = out->width;
    *h = out->height;
    return true;
}

/* ------------------------------------------------------------------ */
/* Desktop background                                                 */
/* ------------------------------------------------------------------ */

/*
 * Startup-gap fallback background.
 *
 * The real desktop/wallpaper is now drawn by desktop.exe (a full-screen
 * Wine window pinned to the bottom of the scene graph).  This rect exists
 * only to avoid a black screen during the brief gap between compositor
 * start and desktop.exe mapping.  Once the desktop window maps and is
 * lowered to the bottom, it covers this rect, so the rect is no longer
 * visible.
 *
 * Color is matched to desktop.exe's hard-coded light purple
 * (RGB 200,170,235) so the boot transition is seamless rather than a
 * jarring blue-then-purple flash.
 */
void server_update_background(struct fg_server *server) {
    int total_w = 0, total_h = 0;

    /* Union of all output rects — covers the whole layout */
    struct fg_output *out;
    wl_list_for_each(out, &server->outputs, link) {
        struct wlr_output *o = out->wlr_output;
        if (o->width > total_w) total_w = o->width;
        if (o->height > total_h) total_h = o->height;
    }

    if (total_w <= 0 || total_h <= 0) return;

    /* Light purple — matches desktop.exe's wallpaper fill */
    float color[4] = { 200.0f/255.0f, 170.0f/255.0f, 235.0f/255.0f, 1.0f };

    if (!server->background_rect) {
        server->background_rect = wlr_scene_rect_create(
            &server->scene->tree, total_w, total_h, color);
        /* Push to the very bottom of the scene graph so it's behind
         * every window — including the desktop window, which is also
         * lowered to bottom later but maps after this rect, leaving the
         * desktop above the rect. */
        wlr_scene_node_lower_to_bottom(
            &server->background_rect->node);
        wlr_log(WLR_INFO, "Fallback background rect created: %dx%d",
            total_w, total_h);
    } else {
        wlr_scene_rect_set_size(server->background_rect, total_w, total_h);
        wlr_scene_rect_set_color(server->background_rect, color);
        wlr_scene_node_lower_to_bottom(
            &server->background_rect->node);
        wlr_log(WLR_INFO, "Fallback background rect resized: %dx%d",
            total_w, total_h);
    }
}

/* ------------------------------------------------------------------ */
/* New output from backend                                            */
/* ------------------------------------------------------------------ */

void server_new_output(struct wl_listener *listener, void *data) {
    struct fg_server *server =
        wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    }
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    struct fg_output *output = calloc(1, sizeof(*output));
    output->wlr_output = wlr_output;
    output->server = server;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    struct wlr_output_layout_output *lo =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    output->scene_output =
        wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, lo,
        output->scene_output);

    /* (Re-)create or resize the desktop background to cover all outputs */
    server_update_background(server);
}
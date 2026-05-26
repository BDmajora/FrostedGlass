/*
 * fg_cursor_override.c — Wine cursor receiver.
 *
 * Server-side implementation of yetios_cursor_manager_v1.
 *
 * Wine sends one request at boot: set_cursor(surface, hotspot_x, hotspot_y).
 * We copy the pixel data from that surface into a compositor-owned
 * wlr_buffer.  That buffer is stored permanently in server->system_cursor_buffer
 * and applied via wlr_cursor_set_buffer().  It is never freed.
 *
 * No shape IDs.  No surface tracking.  No cache.  One cursor.  Forever.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/util/log.h>

#include "fg_cursor_override.h"
#include "fg_types.h"

/* Generated server protocol header */
#include "yetios-cursor-v1-protocol.h"

/* ------------------------------------------------------------------ */
/* Compositor-owned pixel buffer (same pattern as old boot cursor)    */
/* ------------------------------------------------------------------ */

struct fg_data_buffer {
    struct wlr_buffer base;
    uint32_t *data;
};

static const struct wlr_buffer_impl data_buffer_impl;

static struct fg_data_buffer *data_buffer_from_buffer(struct wlr_buffer *buf) {
    if (buf->impl != &data_buffer_impl) return NULL;
    struct fg_data_buffer *b = wl_container_of(buf, b, base);
    return b;
}

static void data_buffer_destroy(struct wlr_buffer *buf) {
    struct fg_data_buffer *b = data_buffer_from_buffer(buf);
    if (!b) return;
    free(b->data);
    free(b);
}

static bool data_buffer_begin_data_ptr_access(struct wlr_buffer *buf,
    uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    (void)flags;
    struct fg_data_buffer *b = data_buffer_from_buffer(buf);
    if (!b) return false;
    *data = b->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)buf->width * 4;
    return true;
}

static void data_buffer_end_data_ptr_access(struct wlr_buffer *buf) {
    (void)buf;
}

static const struct wlr_buffer_impl data_buffer_impl = {
    .destroy = data_buffer_destroy,
    .begin_data_ptr_access = data_buffer_begin_data_ptr_access,
    .end_data_ptr_access = data_buffer_end_data_ptr_access,
};

/*
 * Copy pixel data from a wlr_surface into a new compositor-owned buffer.
 * Returns a locked wlr_buffer, or NULL on failure.
 */
static struct wlr_buffer *copy_surface_to_buffer(
    struct wlr_surface *surface) {

    if (!surface || !surface->buffer) {
        wlr_log(WLR_ERROR, "CURSOR-RECV: surface has no committed buffer");
        return NULL;
    }

    struct wlr_client_buffer *client_buf = surface->buffer;
    int w = client_buf->base.width;
    int h = client_buf->base.height;

    if (w <= 0 || h <= 0) {
        wlr_log(WLR_ERROR, "CURSOR-RECV: buffer has invalid size %dx%d", w, h);
        return NULL;
    }

    /* Read pixels from the client buffer */
    void *src_data = NULL;
    uint32_t src_format = 0;
    size_t src_stride = 0;

    if (!wlr_buffer_begin_data_ptr_access(&client_buf->base,
            WLR_BUFFER_DATA_PTR_ACCESS_READ,
            &src_data, &src_format, &src_stride)) {
        wlr_log(WLR_ERROR, "CURSOR-RECV: failed to access buffer pixels");
        return NULL;
    }

    /* Allocate our own buffer */
    struct fg_data_buffer *dst = calloc(1, sizeof(*dst));
    if (!dst) {
        wlr_buffer_end_data_ptr_access(&client_buf->base);
        return NULL;
    }

    size_t dst_stride = (size_t)w * 4;
    dst->data = calloc((size_t)w * h, sizeof(uint32_t));
    if (!dst->data) {
        free(dst);
        wlr_buffer_end_data_ptr_access(&client_buf->base);
        return NULL;
    }

    /* Copy row by row (strides may differ) */
    for (int y = 0; y < h; y++) {
        memcpy(
            (uint8_t *)dst->data + y * dst_stride,
            (uint8_t *)src_data + y * src_stride,
            dst_stride < src_stride ? dst_stride : src_stride);
    }

    wlr_buffer_end_data_ptr_access(&client_buf->base);

    wlr_buffer_init(&dst->base, &data_buffer_impl, w, h);

    wlr_log(WLR_INFO,
        "CURSOR-RECV: copied %dx%d cursor pixels into compositor buffer",
        w, h);

    return &dst->base;
}

/* ------------------------------------------------------------------ */
/* Protocol request handler                                           */
/* ------------------------------------------------------------------ */

static void manager_set_cursor(struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *surface_resource,
    int32_t hotspot_x, int32_t hotspot_y) {

    struct fg_cursor_override *co = wl_resource_get_user_data(resource);
    if (!co || !co->server) return;

    struct fg_server *server = co->server;

    struct wlr_surface *surface =
        wlr_surface_from_resource(surface_resource);
    if (!surface) {
        wlr_log(WLR_ERROR, "CURSOR-RECV: invalid surface resource");
        return;
    }

    wlr_log(WLR_INFO,
        "CURSOR-RECV: Wine sent set_cursor — surface=%p hotspot=(%d,%d)",
        (void *)surface, hotspot_x, hotspot_y);

    /* Copy the pixels into a permanent compositor-owned buffer */
    struct wlr_buffer *buf = copy_surface_to_buffer(surface);
    if (!buf) {
        wlr_log(WLR_ERROR,
            "CURSOR-RECV: FAILED to copy cursor — no cursor will be shown!");
        return;
    }

    /* Release old cursor buffer if any */
    if (server->system_cursor_buffer) {
        wlr_buffer_unlock(server->system_cursor_buffer);
    }

    /* Store permanently — this is THE cursor from now on */
    server->system_cursor_buffer = wlr_buffer_lock(buf);
    server->system_cursor_hotspot_x = hotspot_x;
    server->system_cursor_hotspot_y = hotspot_y;
    server->system_cursor_scale = 1.0f;
    server->have_system_cursor = true;

    /* Apply immediately */
    wlr_cursor_set_buffer(server->cursor, server->system_cursor_buffer,
        hotspot_x, hotspot_y, 1.0f);

    /* Drop the creator reference (our lock keeps it alive) */
    wlr_buffer_drop(buf);

    wlr_log(WLR_INFO,
        "CURSOR-RECV: *** Wine cursor is now THE system cursor *** "
        "%dx%d hotspot=(%d,%d) — stored permanently",
        server->system_cursor_buffer->width,
        server->system_cursor_buffer->height,
        hotspot_x, hotspot_y);
}

static const struct yetios_cursor_manager_v1_interface manager_impl = {
    .set_cursor = manager_set_cursor,
};

/* ------------------------------------------------------------------ */
/* Global bind                                                        */
/* ------------------------------------------------------------------ */

static void manager_bind(struct wl_client *client, void *data,
    uint32_t version, uint32_t id) {

    struct fg_cursor_override *co = data;

    struct wl_resource *resource = wl_resource_create(
        client, &yetios_cursor_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(resource, &manager_impl, co, NULL);

    wlr_log(WLR_INFO,
        "CURSOR-RECV: client bound yetios_cursor_manager_v1 — "
        "waiting for set_cursor...");
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

struct fg_cursor_override *cursor_override_init(struct fg_server *server) {
    struct fg_cursor_override *co = calloc(1, sizeof(*co));
    if (!co) return NULL;

    co->server = server;

    co->global = wl_global_create(
        server->display,
        &yetios_cursor_manager_v1_interface,
        1, co, manager_bind);

    if (!co->global) {
        wlr_log(WLR_ERROR, "CURSOR-RECV: failed to create wl_global");
        free(co);
        return NULL;
    }

    wlr_log(WLR_INFO,
        "CURSOR-RECV: yetios_cursor_manager_v1 global created — "
        "compositor will wait for Wine to send cursor");

    return co;
}

void cursor_override_destroy(struct fg_cursor_override *co) {
    if (!co) return;
    if (co->global) wl_global_destroy(co->global);
    free(co);
}
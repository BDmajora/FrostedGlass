/*
 * fg_boot_cursor.c — Embedded Win32-style boot cursor.
 *
 * Builds a wlr_buffer (ARGB8888) from a hard-coded classic Windows arrow
 * and hands it back as a compositor-owned cursor image.  Implemented as a
 * minimal wlr_buffer backed by a heap pixel array exposed through the
 * data-ptr access interface — exactly the shape wlroots expects for a
 * software-uploaded cursor (the same mechanism xcursor themes use), so it
 * works on both the DRM and the nested backends.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <drm_fourcc.h>

#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

#include "fg_boot_cursor.h"

/* ------------------------------------------------------------------ */
/* Arrow bitmap                                                       */
/* ------------------------------------------------------------------ */

/*
 * Classic Windows arrow, authored as ASCII art.  Hotspot is the tip at
 * the top-left (0,0).
 *
 *   'X' = black outline   (opaque)
 *   '.' = white fill      (opaque)
 *   ' ' = transparent
 *
 * All rows are padded to the same width by the builder, so they need not
 * be equal length here.
 */
static const char *const ARROW_ART[] = {
    "X",
    "XX",
    "X.X",
    "X..X",
    "X...X",
    "X....X",
    "X.....X",
    "X......X",
    "X.......X",
    "X........X",
    "X.........X",
    "X..........X",
    "X......XXXXXX",
    "X...X..X",
    "X..XX..X",
    "X.X  X..X",
    "XX   X..X",
    "X     X..X",
    "      X..X",
    "       X..X",
    "       XX",
};

#define ARROW_ROWS ((int)(sizeof(ARROW_ART) / sizeof(ARROW_ART[0])))

/* ARGB8888 (DRM little-endian: 0xAARRGGBB in a host uint32). */
#define PX_TRANSPARENT 0x00000000u
#define PX_BLACK       0xFF000000u
#define PX_WHITE       0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* Minimal data-ptr wlr_buffer                                        */
/* ------------------------------------------------------------------ */

struct fg_data_buffer {
    struct wlr_buffer base;
    uint32_t *data;     /* width*height ARGB8888, owned */
};

static const struct wlr_buffer_impl data_buffer_impl;

static struct fg_data_buffer *data_buffer_from_buffer(
    struct wlr_buffer *buffer) {
    /* Guard: only unwrap buffers that are actually ours. */
    if (buffer->impl != &data_buffer_impl) return NULL;
    struct fg_data_buffer *b = wl_container_of(buffer, b, base);
    return b;
}

static void data_buffer_destroy(struct wlr_buffer *buffer) {
    struct fg_data_buffer *b = data_buffer_from_buffer(buffer);
    if (!b) return;
    free(b->data);
    free(b);
}

static bool data_buffer_begin_data_ptr_access(struct wlr_buffer *buffer,
    uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    (void)flags;
    struct fg_data_buffer *b = data_buffer_from_buffer(buffer);
    if (!b) return false;
    *data = b->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)buffer->width * 4;
    return true;
}

static void data_buffer_end_data_ptr_access(struct wlr_buffer *buffer) {
    (void)buffer;
}

static const struct wlr_buffer_impl data_buffer_impl = {
    .destroy = data_buffer_destroy,
    .begin_data_ptr_access = data_buffer_begin_data_ptr_access,
    .end_data_ptr_access = data_buffer_end_data_ptr_access,
};

/* ------------------------------------------------------------------ */
/* Builder                                                            */
/* ------------------------------------------------------------------ */

struct wlr_buffer *fg_boot_cursor_create(int *hotspot_x, int *hotspot_y) {
    /* Compute width as the longest art row. */
    int width = 0;
    for (int y = 0; y < ARROW_ROWS; y++) {
        int len = (int)strlen(ARROW_ART[y]);
        if (len > width) width = len;
    }
    int height = ARROW_ROWS;
    if (width <= 0 || height <= 0) return NULL;

    struct fg_data_buffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->data = calloc((size_t)width * height, sizeof(uint32_t));
    if (!b->data) {
        free(b);
        return NULL;
    }

    /* Rasterize the art into ARGB8888. calloc already zeroed (transparent),
     * so we only need to write the opaque pixels. */
    for (int y = 0; y < height; y++) {
        const char *row = ARROW_ART[y];
        int len = (int)strlen(row);
        for (int x = 0; x < len; x++) {
            uint32_t px;
            switch (row[x]) {
            case 'X': px = PX_BLACK; break;
            case '.': px = PX_WHITE; break;
            default:  px = PX_TRANSPARENT; break;
            }
            if (px != PX_TRANSPARENT) {
                b->data[(size_t)y * width + x] = px;
            }
        }
    }

    wlr_buffer_init(&b->base, &data_buffer_impl, width, height);

    if (hotspot_x) *hotspot_x = 0;
    if (hotspot_y) *hotspot_y = 0;

    wlr_log(WLR_INFO,
        "Boot cursor: built embedded Win32 arrow %dx%d (hotspot 0,0)",
        width, height);

    return &b->base;
}
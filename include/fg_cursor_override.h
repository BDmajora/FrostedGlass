/*
 * fg_cursor_override.h — Win32 cursor override subsystem.
 *
 * Implements the yetios_cursor_manager_v1 protocol server-side.
 * Wine uploads authentic Win32 cursor bitmaps via dedicated wl_surfaces;
 * the compositor caches them and silently overrides every non-Wine
 * client's cursor requests with the cached Win32 images.
 *
 * This is the compositor half of the cursor bridge.  The Wine half
 * (patching waylanddrv or a standalone uploader) sends the cursor
 * data through the protocol defined in yetios-cursor-v1.xml.
 */

#ifndef FG_CURSOR_OVERRIDE_H
#define FG_CURSOR_OVERRIDE_H

#include <stdbool.h>
#include <wayland-server-core.h>

/* Forward declarations — avoid pulling in all of fg_types.h */
struct fg_server;
struct wlr_surface;

/* ------------------------------------------------------------------ */
/* Win32 cursor shape IDs (from winuser.h)                            */
/* ------------------------------------------------------------------ */

#define FG_IDC_ARROW       32512
#define FG_IDC_IBEAM       32513
#define FG_IDC_WAIT        32514
#define FG_IDC_CROSS       32515
#define FG_IDC_UPARROW     32516
#define FG_IDC_SIZENWSE    32642
#define FG_IDC_SIZENESW    32643
#define FG_IDC_SIZEWE      32644
#define FG_IDC_SIZENS      32645
#define FG_IDC_SIZEALL     32646
#define FG_IDC_NO          32648
#define FG_IDC_HAND        32649
#define FG_IDC_APPSTARTING 32650
#define FG_IDC_HELP        32651

/*
 * Maximum number of cached cursor shapes.  Shape IDs are sparse
 * (32512–32651), so we use a flat lookup table indexed by
 * (shape_id - FG_IDC_ARROW).  The range 32512..32651 = 140 entries,
 * which is small enough for a flat array.
 */
#define FG_CURSOR_SHAPE_BASE  FG_IDC_ARROW
#define FG_CURSOR_SHAPE_MAX   (FG_IDC_HELP + 1)
#define FG_CURSOR_SHAPE_COUNT (FG_CURSOR_SHAPE_MAX - FG_CURSOR_SHAPE_BASE)

/* ------------------------------------------------------------------ */
/* Per-shape cache entry                                              */
/* ------------------------------------------------------------------ */

struct fg_cached_cursor {
    struct wlr_surface *surface;    /* Wine's cursor surface, or NULL  */
    int hotspot_x;
    int hotspot_y;
    struct wl_listener destroy;     /* cleans up if surface is destroyed */
    struct fg_cursor_override *parent;  /* back-pointer for destroy callback */
    bool valid;
};

/* ------------------------------------------------------------------ */
/* Cursor override subsystem state                                    */
/* ------------------------------------------------------------------ */

struct fg_cursor_override {
    struct fg_server *server;

    /* The wl_global for yetios_cursor_manager_v1 */
    struct wl_global *global;

    /* The single provider client (Wine), or NULL */
    struct wl_client *provider;
    struct wl_resource *provider_resource;
    struct wl_listener provider_destroy;

    /* Cached cursor shapes — sparse array indexed by
     * (shape_id - FG_CURSOR_SHAPE_BASE) */
    struct fg_cached_cursor shapes[FG_CURSOR_SHAPE_COUNT];

    /* Count of valid cached shapes — quick check for "any cursors?" */
    int cached_count;
};

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

/*
 * Initialize the cursor override subsystem.
 * Creates the yetios_cursor_manager_v1 wl_global on the display.
 * Must be called after wl_display_create() but before the event loop.
 * Returns the subsystem state, or NULL on failure.
 */
struct fg_cursor_override *cursor_override_init(struct fg_server *server);

/*
 * Tear down the cursor override subsystem.
 * Safe to call with NULL.
 */
void cursor_override_destroy(struct fg_cursor_override *co);

/*
 * Check whether a wl_client is the registered cursor provider (Wine).
 * Returns true if the client is Wine and should NOT be hijacked.
 */
bool cursor_override_is_provider(struct fg_cursor_override *co,
                                 struct wl_client *client);

/*
 * Look up the cached Win32 cursor for a given shape ID.
 * Returns true and fills surface/hotspot if a cached cursor exists.
 * Returns false if no cursor is cached for that shape.
 */
bool cursor_override_get(struct fg_cursor_override *co,
                         uint32_t shape_id,
                         struct wlr_surface **surface,
                         int *hotspot_x, int *hotspot_y);

/*
 * Get the default override cursor (IDC_ARROW).
 * Convenience wrapper around cursor_override_get().
 */
bool cursor_override_get_default(struct fg_cursor_override *co,
                                 struct wlr_surface **surface,
                                 int *hotspot_x, int *hotspot_y);

/*
 * Check whether ANY cached cursors exist.
 * Fast path — avoids a shape lookup when the cache is empty.
 */
bool cursor_override_active(struct fg_cursor_override *co);

#endif /* FG_CURSOR_OVERRIDE_H */
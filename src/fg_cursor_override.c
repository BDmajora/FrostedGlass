/*
 * fg_cursor_override.c — Win32 cursor override subsystem.
 *
 * Server-side implementation of the yetios_cursor_manager_v1 protocol.
 *
 * Wine uploads authentic Win32 cursor bitmaps (arrow, I-beam, resize
 * handles, hourglass, etc.) via dedicated wl_surfaces.  This module
 * caches those surfaces and provides a lookup API so that fg_input.c
 * can silently replace any non-Wine client's cursor with the cached
 * Win32 image.
 *
 * Lifecycle:
 *   1. cursor_override_init() creates the wl_global.
 *   2. Wine binds → becomes the provider.
 *   3. Wine calls set_cursor_image() for each shape → we cache them.
 *   4. fg_input.c calls cursor_override_get() on every seat_request_cursor.
 *   5. Wine disconnects → all caches cleared, normal cursors resume.
 *
 * Thread safety: all calls happen on the Wayland event loop thread.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>

#include "fg_cursor_override.h"
#include "fg_types.h"

/* Generated protocol header — built by wayland-scanner from
 * protocol/yetios-cursor-v1.xml */
#include "yetios-cursor-v1-protocol.h"

/* ------------------------------------------------------------------ */
/* Shape index helpers                                                */
/* ------------------------------------------------------------------ */

static inline bool shape_id_valid(uint32_t shape_id) {
    return shape_id >= FG_CURSOR_SHAPE_BASE &&
           shape_id < FG_CURSOR_SHAPE_MAX;
}

static inline int shape_index(uint32_t shape_id) {
    return (int)(shape_id - FG_CURSOR_SHAPE_BASE);
}

static const char *shape_name(uint32_t shape_id) {
    switch (shape_id) {
    case FG_IDC_ARROW:       return "IDC_ARROW";
    case FG_IDC_IBEAM:       return "IDC_IBEAM";
    case FG_IDC_WAIT:        return "IDC_WAIT";
    case FG_IDC_CROSS:       return "IDC_CROSS";
    case FG_IDC_UPARROW:     return "IDC_UPARROW";
    case FG_IDC_SIZENWSE:    return "IDC_SIZENWSE";
    case FG_IDC_SIZENESW:    return "IDC_SIZENESW";
    case FG_IDC_SIZEWE:      return "IDC_SIZEWE";
    case FG_IDC_SIZENS:      return "IDC_SIZENS";
    case FG_IDC_SIZEALL:     return "IDC_SIZEALL";
    case FG_IDC_NO:          return "IDC_NO";
    case FG_IDC_HAND:        return "IDC_HAND";
    case FG_IDC_APPSTARTING: return "IDC_APPSTARTING";
    case FG_IDC_HELP:        return "IDC_HELP";
    default:                 return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/* Cache management                                                   */
/* ------------------------------------------------------------------ */

/*
 * Called when a cached cursor surface is destroyed by Wine (e.g. Wine
 * exits or recreates the surface).  We must clear our reference so we
 * don't use-after-free.
 */
static void cached_surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct fg_cached_cursor *cc =
        wl_container_of(listener, cc, destroy);

    wlr_log(WLR_INFO, "Cursor override: cached surface destroyed");

    wl_list_remove(&cc->destroy.link);
    wl_list_init(&cc->destroy.link);
    cc->surface = NULL;
    cc->valid = false;

    if (cc->parent)
        cc->parent->cached_count--;
}

static void cache_set(struct fg_cursor_override *co, uint32_t shape_id,
                      struct wlr_surface *surface, int hx, int hy) {
    int idx = shape_index(shape_id);
    struct fg_cached_cursor *cc = &co->shapes[idx];

    /* Remove old listener if replacing */
    if (cc->valid && cc->surface) {
        wl_list_remove(&cc->destroy.link);
        wl_list_init(&cc->destroy.link);
    }

    bool was_valid = cc->valid;

    cc->surface = surface;
    cc->hotspot_x = hx;
    cc->hotspot_y = hy;
    cc->valid = true;
    cc->parent = co;

    /* Listen for surface destruction */
    cc->destroy.notify = cached_surface_destroy;
    wl_signal_add(&surface->events.destroy, &cc->destroy);

    if (!was_valid)
        co->cached_count++;

    wlr_log(WLR_INFO,
        "Cursor override: cached shape %s (%u) hotspot=(%d,%d) [%d total]",
        shape_name(shape_id), shape_id, hx, hy, co->cached_count);
}

static void cache_remove(struct fg_cursor_override *co, uint32_t shape_id) {
    int idx = shape_index(shape_id);
    struct fg_cached_cursor *cc = &co->shapes[idx];

    if (!cc->valid) return;

    if (cc->surface) {
        wl_list_remove(&cc->destroy.link);
        wl_list_init(&cc->destroy.link);
    }

    cc->surface = NULL;
    cc->valid = false;
    co->cached_count--;

    wlr_log(WLR_INFO,
        "Cursor override: removed shape %s (%u) [%d total]",
        shape_name(shape_id), shape_id, co->cached_count);
}

static void cache_clear_all(struct fg_cursor_override *co) {
    for (int i = 0; i < FG_CURSOR_SHAPE_COUNT; i++) {
        struct fg_cached_cursor *cc = &co->shapes[i];
        if (cc->valid) {
            if (cc->surface) {
                wl_list_remove(&cc->destroy.link);
                wl_list_init(&cc->destroy.link);
            }
            cc->surface = NULL;
            cc->valid = false;
        }
    }
    co->cached_count = 0;
    wlr_log(WLR_INFO, "Cursor override: all cached cursors cleared");
}

/* ------------------------------------------------------------------ */
/* Protocol request handlers                                          */
/* ------------------------------------------------------------------ */

static void manager_set_cursor_image(struct wl_client *client,
    struct wl_resource *resource,
    uint32_t shape_id,
    struct wl_resource *surface_resource,
    int32_t hotspot_x, int32_t hotspot_y) {

    struct fg_cursor_override *co = wl_resource_get_user_data(resource);
    if (!co) return;

    /* Validate shape ID is in the Win32 range */
    if (!shape_id_valid(shape_id)) {
        wlr_log(WLR_ERROR,
            "Cursor override: invalid shape_id %u (valid range %u–%u)",
            shape_id, FG_CURSOR_SHAPE_BASE, FG_CURSOR_SHAPE_MAX - 1);
        wl_resource_post_error(resource,
            WL_DISPLAY_ERROR_INVALID_OBJECT,
            "shape_id %u out of range", shape_id);
        return;
    }

    /* Get the wlr_surface from the wl_resource */
    struct wlr_surface *surface =
        wlr_surface_from_resource(surface_resource);
    if (!surface) {
        wlr_log(WLR_ERROR,
            "Cursor override: surface resource is not a valid wlr_surface");
        return;
    }

    cache_set(co, shape_id, surface, hotspot_x, hotspot_y);
}

static void manager_remove_cursor_image(struct wl_client *client,
    struct wl_resource *resource, uint32_t shape_id) {

    struct fg_cursor_override *co = wl_resource_get_user_data(resource);
    if (!co) return;

    if (!shape_id_valid(shape_id)) return;

    cache_remove(co, shape_id);
}

static void manager_destroy(struct wl_client *client,
    struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct yetios_cursor_manager_v1_interface manager_impl = {
    .set_cursor_image = manager_set_cursor_image,
    .remove_cursor_image = manager_remove_cursor_image,
    .destroy = manager_destroy,
};

/* ------------------------------------------------------------------ */
/* Resource lifecycle                                                 */
/* ------------------------------------------------------------------ */

/*
 * Called when the provider's wl_resource is destroyed (either by
 * explicit destroy request or client disconnect).  Clears all state.
 */
static void manager_resource_destroy(struct wl_resource *resource) {
    struct fg_cursor_override *co = wl_resource_get_user_data(resource);
    if (!co) return;

    wlr_log(WLR_INFO,
        "Cursor override: provider disconnected, clearing all cursors");

    cache_clear_all(co);
    co->provider = NULL;
    co->provider_resource = NULL;

    if (co->provider_destroy.link.next)
        wl_list_remove(&co->provider_destroy.link);
    wl_list_init(&co->provider_destroy.link);
}

/*
 * Called when the provider wl_client itself is destroyed.
 * The resource destructor handles cleanup; this is belt-and-suspenders.
 */
static void provider_client_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct fg_cursor_override *co =
        wl_container_of(listener, co, provider_destroy);

    wlr_log(WLR_INFO,
        "Cursor override: provider client destroyed");

    cache_clear_all(co);
    co->provider = NULL;
    co->provider_resource = NULL;

    wl_list_remove(&co->provider_destroy.link);
    wl_list_init(&co->provider_destroy.link);
}

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

    /*
     * Only one provider at a time.  If Wine reconnects (e.g. after a
     * crash), the old provider is already gone (its resource was
     * destroyed), so this slot is free.  If somehow two clients try
     * to bind, the second one gets a functional resource but its
     * uploads will be ignored (we could also reject it, but being
     * permissive is simpler and more robust).
     */
    if (co->provider && co->provider != client) {
        wlr_log(WLR_INFO,
            "Cursor override: replacing previous provider with new client");
        cache_clear_all(co);
        if (co->provider_destroy.link.next)
            wl_list_remove(&co->provider_destroy.link);
        wl_list_init(&co->provider_destroy.link);
    }

    co->provider = client;
    co->provider_resource = resource;

    wl_resource_set_implementation(resource, &manager_impl,
        co, manager_resource_destroy);

    /* Watch for provider client destruction */
    co->provider_destroy.notify = provider_client_destroy;
    wl_client_add_destroy_listener(client, &co->provider_destroy);

    wlr_log(WLR_INFO,
        "Cursor override: provider registered (client %p)", (void *)client);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

struct fg_cursor_override *cursor_override_init(struct fg_server *server) {
    struct fg_cursor_override *co = calloc(1, sizeof(*co));
    if (!co) return NULL;

    co->server = server;
    wl_list_init(&co->provider_destroy.link);

    /* Initialize all cache entries */
    for (int i = 0; i < FG_CURSOR_SHAPE_COUNT; i++) {
        co->shapes[i].valid = false;
        co->shapes[i].surface = NULL;
        wl_list_init(&co->shapes[i].destroy.link);
    }

    co->global = wl_global_create(
        server->display,
        &yetios_cursor_manager_v1_interface,
        1,                  /* version */
        co,                 /* data */
        manager_bind);

    if (!co->global) {
        wlr_log(WLR_ERROR,
            "Cursor override: failed to create wl_global");
        free(co);
        return NULL;
    }

    wlr_log(WLR_INFO,
        "Cursor override: yetios_cursor_manager_v1 global created — "
        "waiting for Wine to register as provider");

    return co;
}

void cursor_override_destroy(struct fg_cursor_override *co) {
    if (!co) return;

    cache_clear_all(co);

    if (co->provider_destroy.link.next &&
        co->provider_destroy.link.next != &co->provider_destroy.link) {
        wl_list_remove(&co->provider_destroy.link);
    }

    if (co->global) {
        wl_global_destroy(co->global);
    }

    free(co);
}

bool cursor_override_is_provider(struct fg_cursor_override *co,
                                 struct wl_client *client) {
    if (!co || !co->provider) return false;
    return co->provider == client;
}

bool cursor_override_get(struct fg_cursor_override *co,
                         uint32_t shape_id,
                         struct wlr_surface **surface,
                         int *hotspot_x, int *hotspot_y) {
    if (!co) return false;
    if (!shape_id_valid(shape_id)) return false;

    int idx = shape_index(shape_id);
    struct fg_cached_cursor *cc = &co->shapes[idx];

    if (!cc->valid || !cc->surface) return false;

    *surface = cc->surface;
    *hotspot_x = cc->hotspot_x;
    *hotspot_y = cc->hotspot_y;
    return true;
}

bool cursor_override_get_default(struct fg_cursor_override *co,
                                 struct wlr_surface **surface,
                                 int *hotspot_x, int *hotspot_y) {
    return cursor_override_get(co, FG_IDC_ARROW, surface, hotspot_x, hotspot_y);
}

bool cursor_override_active(struct fg_cursor_override *co) {
    if (!co) return false;
    return co->cached_count > 0;
}
// protocol.c - Core Wayland protocol implementation
#include "compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

// wl_compositor interface
static void compositor_create_surface(struct wl_client *client,
                                     struct wl_resource *resource,
                                     uint32_t id) {
    triangles_surface_create(client, resource, id);
}

static void compositor_create_region(struct wl_client *client,
                                    struct wl_resource *resource,
                                    uint32_t id) {
    // TODO: Implement region support
    struct wl_resource *region_resource = wl_resource_create(client,
        &wl_region_interface, wl_resource_get_version(resource), id);
    
    if (!region_resource) {
        wl_client_post_no_memory(client);
        return;
    }
}

static const struct wl_compositor_interface compositor_interface = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data,
                           uint32_t version, uint32_t id) {
    struct triangles_compositor *compositor = data;
    struct wl_resource *resource;
    
    resource = wl_resource_create(client, &wl_compositor_interface,
                                  version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    
    wl_resource_set_implementation(resource, &compositor_interface,
                                   compositor, NULL);
}

// wl_shm (shared memory) support
// libwayland handles all of this for us via wl_display_init_shm!

// wl_subcompositor interface (for subsurfaces)
static void subcompositor_destroy(struct wl_client *client,
                                 struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void subcompositor_get_subsurface(struct wl_client *client,
                                        struct wl_resource *resource,
                                        uint32_t id,
                                        struct wl_resource *surface,
                                        struct wl_resource *parent) {
    // TODO: Implement subsurface support
    struct wl_resource *subsurface_resource = wl_resource_create(client,
        &wl_subsurface_interface, 1, id);
    
    if (!subsurface_resource) {
        wl_client_post_no_memory(client);
        return;
    }
}

static const struct wl_subcompositor_interface subcompositor_interface = {
    .destroy = subcompositor_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void subcompositor_bind(struct wl_client *client, void *data,
                              uint32_t version, uint32_t id) {
    struct wl_resource *resource;
    
    resource = wl_resource_create(client, &wl_subcompositor_interface,
                                  version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    
    wl_resource_set_implementation(resource, &subcompositor_interface,
                                   NULL, NULL);
}

// wl_seat interface
static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource,
                            uint32_t id) {
    struct wl_resource *pointer_resource = wl_resource_create(client,
        &wl_pointer_interface, wl_resource_get_version(resource), id);
    
    if (!pointer_resource) {
        wl_client_post_no_memory(client);
        return;
    }
    
    wl_resource_set_implementation(pointer_resource, NULL, NULL, NULL);
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource,
                             uint32_t id) {
    struct wl_resource *keyboard_resource = wl_resource_create(client,
        &wl_keyboard_interface, wl_resource_get_version(resource), id);
    
    if (!keyboard_resource) {
        wl_client_post_no_memory(client);
        return;
    }
    
    wl_resource_set_implementation(keyboard_resource, NULL, NULL, NULL);
    
    // TODO: Send keymap
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *resource,
                          uint32_t id) {
    struct wl_resource *touch_resource = wl_resource_create(client,
        &wl_touch_interface, wl_resource_get_version(resource), id);
    
    if (!touch_resource) {
        wl_client_post_no_memory(client);
        return;
    }
    
    wl_resource_set_implementation(touch_resource, NULL, NULL, NULL);
}

static void seat_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct wl_seat_interface seat_interface = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static void seat_bind(struct wl_client *client, void *data,
                     uint32_t version, uint32_t id) {
    struct triangles_seat *seat = data;
    struct wl_resource *resource;
    
    resource = wl_resource_create(client, &wl_seat_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    
    wl_resource_set_implementation(resource, &seat_interface, seat, NULL);
    
    // Send capabilities
    wl_seat_send_capabilities(resource, seat->capabilities);
    
    if (version >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(resource, seat->name);
    }
}

bool triangles_compositor_protocol_init(struct triangles_compositor *compositor) {
    // Create wl_compositor global
    compositor->compositor_global = wl_global_create(compositor->display,
        &wl_compositor_interface, 4, compositor, compositor_bind);
    if (!compositor->compositor_global) {
        return false;
    }
    
    // Create wl_subcompositor global
    compositor->subcompositor_global = wl_global_create(compositor->display,
        &wl_subcompositor_interface, 1, compositor, subcompositor_bind);
    if (!compositor->subcompositor_global) {
        return false;
    }
    
    // Initialize built-in SHM support - libwayland handles everything!
    printf("[PROTOCOL] Initializing wl_shm...\n");
    fflush(stdout);
    
    if (wl_display_init_shm(compositor->display) < 0) {
        fprintf(stderr, "[PROTOCOL] Failed to init SHM\n");
        return false;
    }
    
    printf("[PROTOCOL] wl_shm initialized (libwayland built-in)\n");
    fflush(stdout);
    
    // Add supported formats
    uint32_t formats[] = { WL_SHM_FORMAT_ARGB8888, WL_SHM_FORMAT_XRGB8888 };
    for (size_t i = 0; i < sizeof(formats)/sizeof(formats[0]); i++) {
        if (wl_display_add_shm_format(compositor->display, formats[i]) != 0) {
            fprintf(stderr, "[PROTOCOL] Failed to add SHM format %u\n", formats[i]);
        }
    }
    
    // Create default seat
    struct triangles_seat *seat = calloc(1, sizeof(*seat));
    if (!seat) {
        return false;
    }
    
    seat->compositor = compositor;
    seat->name = strdup("seat0");
    seat->capabilities = WL_SEAT_CAPABILITY_POINTER | 
                        WL_SEAT_CAPABILITY_KEYBOARD;
    
    wl_list_init(&seat->resources);
    
    seat->global = wl_global_create(compositor->display,
        &wl_seat_interface, 7, seat, seat_bind);
    if (!seat->global) {
        free(seat->name);
        free(seat);
        return false;
    }
    
    wl_list_insert(&compositor->seat_list, &seat->link);
    
    printf("Core Wayland protocols initialized\n");
    return true;
}

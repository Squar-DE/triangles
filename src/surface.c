// surface.c - Surface and view management
#include "compositor.h"
#include <bits/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-util.h>

extern GLuint triangles_renderer_create_texture(struct triangles_surface *surface,
                                               struct wl_resource *buffer);

static void surface_buffer_destroy_handler(struct wl_listener *listener, void *data) {
    struct triangles_surface *surface = wl_container_of(listener, surface, buffer_destroy_listener);
    surface->buffer_resource = NULL;
    surface->has_buffer = false;
}

// Surface interface implementation
static void surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource,
                          struct wl_resource *buffer, int32_t x, int32_t y) {
    struct triangles_surface *surface = wl_resource_get_user_data(resource);
    
    printf("[SURFACE] Attach called: buffer=%p, x=%d, y=%d\n", (void*)buffer, x, y);
    fflush(stdout);
    
    // Store pending buffer, don't apply yet
    surface->pending_buffer = buffer;
}

static void surface_damage(struct wl_client *client, struct wl_resource *resource,
                          int32_t x, int32_t y, int32_t width, int32_t height) {
    struct triangles_surface *surface = wl_resource_get_user_data(resource);
    pixman_region32_union_rect(&surface->damage, &surface->damage, x, y, width, height);
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource,
                         uint32_t callback) {
    struct triangles_surface *surface = wl_resource_get_user_data(resource);
    
    struct wl_resource *callback_resource = wl_resource_create(client,
        &wl_callback_interface, 1, callback);
    
    if (!callback_resource) {
        wl_client_post_no_memory(client);
        return;
    }
    
    wl_list_insert(surface->frame_callbacks.prev, 
                   wl_resource_get_link(callback_resource));
}

static void surface_set_opaque_region(struct wl_client *client,
                                     struct wl_resource *resource,
                                     struct wl_resource *region) {
    struct triangles_surface *surface = wl_resource_get_user_data(resource);
    
    if (region) {
        // TODO: Copy region data
    } else {
        pixman_region32_clear(&surface->opaque);
    }
}

static void surface_set_input_region(struct wl_client *client,
                                    struct wl_resource *resource,
                                    struct wl_resource *region) {
    struct triangles_surface *surface = wl_resource_get_user_data(resource);
    
    if (region) {
        // TODO: Copy region data
    } else {
        pixman_region32_clear(&surface->input);
    }
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
    struct triangles_surface *surface = wl_resource_get_user_data(resource);
    triangles_surface_commit(surface);
}

static void surface_set_buffer_transform(struct wl_client *client,
                                        struct wl_resource *resource,
                                        int32_t transform) {
    // TODO: Handle buffer transforms
}

static void surface_set_buffer_scale(struct wl_client *client,
                                    struct wl_resource *resource,
                                    int32_t scale) {
    struct triangles_surface *surface = wl_resource_get_user_data(resource);
    
    // Store surface scale for HiDPI support
    if (scale > 0) {
        surface->scale = (double)scale;
    }
}

static void surface_damage_buffer(struct wl_client *client,
                                 struct wl_resource *resource,
                                 int32_t x, int32_t y,
                                 int32_t width, int32_t height) {
    surface_damage(client, resource, x, y, width, height);
}

static const struct wl_surface_interface surface_interface = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
};

static void surface_resource_destroy(struct wl_resource *resource) {
    struct triangles_surface *surface = wl_resource_get_user_data(resource);
    triangles_surface_destroy(surface);
}

struct triangles_surface *triangles_surface_create(struct wl_client *client,
                                                    struct wl_resource *compositor_resource,
                                                    uint32_t id) {
    struct triangles_compositor *compositor = wl_resource_get_user_data(compositor_resource);
    
    struct triangles_surface *surface = calloc(1, sizeof(*surface));
    if (!surface) {
        wl_client_post_no_memory(client);
        return NULL;
    }
    
    surface->compositor = compositor;
    surface->resource = wl_resource_create(client, &wl_surface_interface,
                                          wl_resource_get_version(compositor_resource), id);
    if (!surface->resource) {
        free(surface);
        wl_client_post_no_memory(client);
        return NULL;
    }
    
    wl_resource_set_implementation(surface->resource, &surface_interface,
                                   surface, surface_resource_destroy);
    
    // Initialize regions
    pixman_region32_init(&surface->damage);
    pixman_region32_init(&surface->opaque);
    pixman_region32_init(&surface->input);
    
    // Initialize frame callback list
    wl_list_init(&surface->frame_callbacks);
    
    // Initialize buffer listener
    surface->buffer_destroy_listener.notify = surface_buffer_destroy_handler;
    
    // Default scale
    surface->scale = 1.0;
    
    wl_list_insert(&compositor->surface_list, &surface->link);
    
    return surface;
}

void triangles_surface_destroy(struct triangles_surface *surface) {
    if (!surface) return;

    struct triangles_view *view, *v_tmp;
    wl_list_for_each_safe(view, v_tmp, &surface->compositor->view_list, link) {
        if (view->surface == surface) {
            triangles_view_destroy(view);
        }
    }
    
    // Cleanup buffer
    if (surface->buffer_resource) {
        wl_list_remove(&surface->buffer_destroy_listener.link);
    }
    
    // Cleanup texture
    if (surface->texture) {
        glDeleteTextures(1, &surface->texture);
    }
    
    // Cleanup regions
    pixman_region32_fini(&surface->damage);
    pixman_region32_fini(&surface->opaque);
    pixman_region32_fini(&surface->input);
    
    // Send frame callbacks
    struct wl_resource *callback, *tmp;
    wl_resource_for_each_safe(callback, tmp, &surface->frame_callbacks) {
        wl_callback_send_done(callback, 0);
        wl_resource_destroy(callback);
    }
    
    wl_list_remove(&surface->link);
    free(surface);
}

void triangles_surface_attach_buffer(struct triangles_surface *surface,
                                      struct wl_resource *buffer) {
    printf("[SURFACE] attach_buffer called, buffer=%p\n", (void*)buffer);
    fflush(stdout);
    
    if (!buffer) {
        printf("[SURFACE] NULL buffer, clearing surface\n");
        fflush(stdout);
        surface->has_buffer = false;
        return;
    }
    
    struct wl_shm_buffer *shm_buffer = wl_shm_buffer_get(buffer);
    printf("[SURFACE] wl_shm_buffer_get returned: %p\n", (void*)shm_buffer);
    fflush(stdout);
    
    if (shm_buffer) {
        surface->buffer_width = wl_shm_buffer_get_width(shm_buffer);
        surface->buffer_height = wl_shm_buffer_get_height(shm_buffer);
        surface->buffer_format = wl_shm_buffer_get_format(shm_buffer);
        
        printf("[SURFACE] SHM buffer info: %dx%d format=%u\n",
               surface->buffer_width, surface->buffer_height, surface->buffer_format);
        fflush(stdout);
    } else {
        printf("[SURFACE] Not a SHM buffer (DMA-BUF?)\n");
        fflush(stdout);
    }
    
    // Create/update texture
    if (surface->texture) {
        printf("[SURFACE] Deleting old texture %u\n", surface->texture);
        fflush(stdout);
        glDeleteTextures(1, &surface->texture);
    }
    
    printf("[SURFACE] Creating texture from buffer...\n");
    fflush(stdout);
    surface->texture = triangles_renderer_create_texture(surface, buffer);
    surface->has_buffer = (surface->texture != 0);
    
    printf("[SURFACE] Texture creation result: texture=%u, has_buffer=%d\n",
           surface->texture, surface->has_buffer);
    fflush(stdout);
}

void triangles_surface_commit(struct triangles_surface *surface) {
    printf("[COMMIT] Surface commit\n");
    fflush(stdout);
    
    // Release old buffer before attaching new one
    if (surface->buffer_resource && surface->pending_buffer && 
        surface->buffer_resource != surface->pending_buffer) {
        printf("[COMMIT] Releasing old buffer\n");
        fflush(stdout);
        // Release the old buffer since we're replacing it
        wl_buffer_send_release(surface->buffer_resource);
    }
    
    // Attach pending buffer
    if (surface->pending_buffer) {
        printf("[COMMIT] Attaching new buffer\n");
        fflush(stdout);
        
        if (surface->buffer_resource) {
            wl_list_remove(&surface->buffer_destroy_listener.link);
        }
        
        surface->buffer_resource = surface->pending_buffer;
        surface->pending_buffer = NULL;
        
        wl_resource_add_destroy_listener(surface->buffer_resource, 
                                        &surface->buffer_destroy_listener);
        
        triangles_surface_attach_buffer(surface, surface->buffer_resource);
    }
    
    // Call role-specific commit handler
    if (surface->commit_handler && surface->role_data) {
        printf("[COMMIT] Calling role handler\n");
        fflush(stdout);
        surface->commit_handler(surface->role_data);
    }
    
    // Count frame callbacks
    int callback_count = 0;
    struct wl_resource *callback, *tmp;
    wl_resource_for_each_safe(callback, tmp, &surface->frame_callbacks) {
        callback_count++;
    }
    
    printf("[COMMIT] Sending %d frame callbacks\n", callback_count);
    fflush(stdout);
    
    // Send frame callbacks - tell client it can render next frame
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint32_t time_ms = now.tv_sec * 1000 + now.tv_nsec / 1000000; // TODO: Use actual timestamp
    wl_resource_for_each_safe(callback, tmp, &surface->frame_callbacks) {
        wl_callback_send_done(callback, time_ms);
        wl_resource_destroy(callback);
    }
    wl_list_init(&surface->frame_callbacks);
    
    // Clear damage (we've processed it)
    pixman_region32_clear(&surface->damage);
    
    // Trigger repaint on all outputs showing this surface
    struct triangles_view *view;
    wl_list_for_each(view, &surface->compositor->view_list, link) {
        if (view->surface == surface && view->mapped && view->output) {
            printf("[COMMIT] Triggering repaint\n");
            fflush(stdout);
            triangles_output_repaint(view->output);
        }
    }
    
    // IMPORTANT: Release the buffer AFTER rendering so client can reuse it
    // For simple SHM buffers, we can release immediately after texture upload
    if (surface->buffer_resource) {
        printf("[COMMIT] Releasing buffer for reuse\n");
        fflush(stdout);
        wl_buffer_send_release(surface->buffer_resource);
    }
    
    printf("[COMMIT] Commit complete\n");
    fflush(stdout);
}

// View management
struct triangles_view *triangles_view_create(struct triangles_surface *surface) {
    struct triangles_view *view = calloc(1, sizeof(*view));
    if (!view) {
        return NULL;
    }
    
    view->compositor = surface->compositor;
    view->surface = surface;
    view->mapped = false;
    
    wl_list_insert(&surface->compositor->view_list, &view->link);
    
    return view;
}

void triangles_view_destroy(struct triangles_view *view) {
    if (!view) return;

    struct triangles_output *output = view->output;
    wl_list_remove(&view->link);

    if (output) {
        triangles_output_repaint(output);
    }
    
    free(view);
}

void triangles_view_map(struct triangles_view *view, int32_t x, int32_t y) {
    if (!view || !view->surface) return;
    
    view->x = x;
    view->y = y;
    view->width = view->surface->buffer_width;
    view->height = view->surface->buffer_height;
    view->mapped = true;
    
    printf("[VIEW] Mapping view at (%d, %d) size %dx%d\n", x, y, view->width, view->height);
    fflush(stdout);
    
    // Assign to first output if not already assigned
    if (!view->output && !wl_list_empty(&view->compositor->output_list)) {
        view->output = wl_container_of(view->compositor->output_list.next,
                                       view->output, link);
        printf("[VIEW] Assigned to output\n");
        fflush(stdout);
    }
    
    // Trigger repaint
    if (view->output) {
        printf("[VIEW] Triggering repaint for mapped view\n");
        fflush(stdout);
        triangles_output_repaint(view->output);
    }
}

void triangles_view_unmap(struct triangles_view *view) {
    if (!view) return;
    
    view->mapped = false;
    
    // Trigger repaint to remove from screen
    if (view->output) {
        triangles_output_repaint(view->output);
    }
}

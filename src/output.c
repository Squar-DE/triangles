// output.c - Output management with proper fractional scaling
#include "compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Wayland output interface implementation
static void output_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct wl_output_interface output_interface = {
    .release = output_release,
};

static void output_bind(struct wl_client *client, void *data,
                       uint32_t version, uint32_t id) {
    struct triangles_output *output = data;
    struct wl_resource *resource;
    
    resource = wl_resource_create(client, &wl_output_interface,
                                  version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    
    wl_resource_set_implementation(resource, &output_interface,
                                   output, NULL);
    
    // Send output information to client
    wl_output_send_geometry(resource, output->x, output->y,
                           output->mode.hdisplay, output->mode.vdisplay,
                           0, "Triangles", "Compositor", WL_OUTPUT_TRANSFORM_NORMAL);
    
    // Send mode information
    wl_output_send_mode(resource,
                       WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                       output->width, output->height, output->refresh);
    
    // Send scale - round up for legacy clients that don't support fractional
    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION) {
        wl_output_send_scale(resource, output->scale_int);
    }
    
    if (version >= WL_OUTPUT_DONE_SINCE_VERSION) {
        wl_output_send_done(resource);
    }
}

// Per-BO framebuffer cache — stored as GBM BO user data so we only call
// drmModeAddFB once per buffer object, not once per frame.
struct bo_fb {
    uint32_t fb_id;
};

static void bo_fb_destroy(struct gbm_bo *bo, void *data) {
    struct bo_fb *cache = data;
    // Retrieve the drm_fd via the gbm device
    int drm_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
    drmModeRmFB(drm_fd, cache->fb_id);
    free(cache);
}

static uint32_t get_fb_for_bo(int drm_fd, struct gbm_bo *bo) {
    // Return cached fb_id if we already created one for this BO
    struct bo_fb *cache = gbm_bo_get_user_data(bo);
    if (cache) return cache->fb_id;

    uint32_t fb_id;
    int ret = drmModeAddFB(drm_fd,
                           gbm_bo_get_width(bo),
                           gbm_bo_get_height(bo),
                           24, 32,
                           gbm_bo_get_stride(bo),
                           gbm_bo_get_handle(bo).u32,
                           &fb_id);
    if (ret) {
        fprintf(stderr, "[OUTPUT] drmModeAddFB failed: %d\n", ret);
        return 0;
    }

    cache = calloc(1, sizeof(*cache));
    cache->fb_id = fb_id;
    gbm_bo_set_user_data(bo, cache, bo_fb_destroy);
    return fb_id;
}

static void output_handle_drm_flip(int fd, unsigned int sequence,
                                    unsigned int tv_sec, unsigned int tv_usec,
                                    void *user_data) {
    (void)fd; (void)sequence;
    struct triangles_output *output = user_data;
    uint32_t time_ms = tv_sec * 1000 + tv_usec / 1000;

    if (output->current_bo)
        gbm_surface_release_buffer(output->gbm_surface, output->current_bo);

    output->current_bo   = output->next_bo;
    output->fb_id        = output->next_fb_id;
    output->next_bo      = NULL;
    output->next_fb_id   = 0;
    output->flip_pending = false;

    // Fire frame callbacks now that the frame is actually on screen
    struct triangles_compositor *compositor = output->compositor;
    struct triangles_view *view;
    wl_list_for_each(view, &compositor->view_list, link) {
        if (!view->mapped || view->output != output || !view->surface) continue;
        struct wl_resource *cb, *tmp;
        wl_resource_for_each_safe(cb, tmp, &view->surface->frame_callbacks) {
            wl_callback_send_done(cb, time_ms);
            wl_resource_destroy(cb);
        }
        wl_list_init(&view->surface->frame_callbacks);
    }

    if (output->needs_repaint) {
        output->needs_repaint = false;
        triangles_output_repaint(output);
    }
}

struct triangles_output *triangles_output_create(struct triangles_compositor *compositor,
                                                  uint32_t connector_id) {
    printf("      [OUTPUT_CREATE] Starting output creation for connector %u\n", connector_id);
    fflush(stdout);
    
    struct triangles_output *output = calloc(1, sizeof(*output));
    if (!output) {
        fprintf(stderr, "      [OUTPUT_CREATE] Failed to allocate output\n");
        return NULL;
    }
    
    output->compositor = compositor;
    output->connector_id = connector_id;
    
    printf("      [OUTPUT_CREATE] Getting connector info...\n");
    fflush(stdout);
    
    // Get connector information
    drmModeConnector *connector = drmModeGetConnector(compositor->drm_fd, connector_id);
    if (!connector) {
        fprintf(stderr, "      [OUTPUT_CREATE] Failed to get connector\n");
        free(output);
        return NULL;
    }
    
    printf("      [OUTPUT_CREATE] Connector has %d modes\n", connector->count_modes);
    fflush(stdout);
    
    // Use preferred mode
    drmModeModeInfo *mode = &connector->modes[0];
    output->mode = *mode;
    output->width = mode->hdisplay;
    output->height = mode->vdisplay;
    output->refresh = mode->vrefresh * 1000; // Convert to mHz
    
    printf("      [OUTPUT_CREATE] Mode: %dx%d@%dHz\n", 
           output->width, output->height, mode->vrefresh);
    fflush(stdout);
    
    // Set default fractional scale (1.0 = no scaling)
    // This can be adjusted based on DPI or user preference
    output->scale = 1.0;
    output->scale_int = (int32_t)(output->scale + 0.5); // Round to nearest
    
    // Auto-detect scale based on DPI if available
    if (connector->mmWidth > 0 && connector->mmHeight > 0) {
        double dpi_x = (output->width * 25.4) / connector->mmWidth;
        double dpi_y = (output->height * 25.4) / connector->mmHeight;
        double dpi = (dpi_x + dpi_y) / 2.0;
        
        // Scale factors based on DPI (96 DPI = 1.0 scale)
        if (dpi >= 192.0) {
            output->scale = 2.0;
        } else if (dpi >= 144.0) {
            output->scale = 1.5;
        } else if (dpi >= 120.0) {
            output->scale = 1.25;
        }
        
        output->scale_int = (int32_t)(output->scale + 0.5);
        printf("      [OUTPUT_CREATE] Auto-detected DPI: %.1f, scale: %.2f\n", dpi, output->scale);
        fflush(stdout);
    }
    
    printf("      [OUTPUT_CREATE] Finding CRTC...\n");
    fflush(stdout);
    
    // Find encoder and CRTC
    drmModeEncoder *encoder = NULL;
    for (int i = 0; i < connector->count_encoders; i++) {
        encoder = drmModeGetEncoder(compositor->drm_fd, connector->encoders[i]);
        if (encoder && encoder->crtc_id) {
            output->crtc_id = encoder->crtc_id;
            printf("      [OUTPUT_CREATE] Found CRTC %u from encoder\n", output->crtc_id);
            fflush(stdout);
            drmModeFreeEncoder(encoder);
            break;
        }
        if (encoder) {
            drmModeFreeEncoder(encoder);
        }
    }
    
    // If no CRTC assigned, find one
    if (!output->crtc_id) {
        printf("      [OUTPUT_CREATE] No CRTC assigned, finding available one...\n");
        fflush(stdout);
        for (int i = 0; i < compositor->resources->count_crtcs; i++) {
            output->crtc_id = compositor->resources->crtcs[i];
            printf("      [OUTPUT_CREATE] Using CRTC %u\n", output->crtc_id);
            fflush(stdout);
            break;
        }
    }
    
    if (!output->crtc_id) {
        fprintf(stderr, "      [OUTPUT_CREATE] Failed to find CRTC\n");
        drmModeFreeConnector(connector);
        free(output);
        return NULL;
    }
    
    printf("      [OUTPUT_CREATE] Saving current CRTC...\n");
    fflush(stdout);
    
    // Save current CRTC for restoration
    output->saved_crtc = drmModeGetCrtc(compositor->drm_fd, output->crtc_id);
    
    printf("      [OUTPUT_CREATE] Creating GBM surface %dx%d...\n", output->width, output->height);
    fflush(stdout);
    
   // Create GBM surface for this output
// Try ARGB8888 first (more compatible)
output->gbm_surface = gbm_surface_create(compositor->gbm,
                                        output->width, output->height,
                                        GBM_FORMAT_ARGB8888,
                                        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
if (!output->gbm_surface) {
    fprintf(stderr, "      [OUTPUT_CREATE] Failed ARGB8888, trying XRGB8888\n");
    fflush(stdout);
    output->gbm_surface = gbm_surface_create(compositor->gbm,
                                            output->width, output->height,
                                            GBM_FORMAT_XRGB8888,
                                            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
}

if (!output->gbm_surface) {
    fprintf(stderr, "      [OUTPUT_CREATE] Failed to create GBM surface\n");
    drmModeFreeConnector(connector);
    free(output);
    return NULL;
}

printf("      [OUTPUT_CREATE] GBM surface created successfully\n");
fflush(stdout); 
    drmModeFreeConnector(connector);

    // Create the EGL window surface once — reused every frame.
    // GBM requires the surface to persist so it can manage the buffer queue.
    output->egl_surface = eglCreateWindowSurface(
        compositor->egl_display,
        compositor->egl_config,
        (EGLNativeWindowType)output->gbm_surface,
        NULL);
    if (output->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "      [OUTPUT_CREATE] Failed to create EGL surface: 0x%x\n",
                eglGetError());
        gbm_surface_destroy(output->gbm_surface);
        free(output);
        return NULL;
    }
    printf("      [OUTPUT_CREATE] EGL surface created\n");
    fflush(stdout);
    
    printf("      [OUTPUT_CREATE] Creating Wayland global...\n");
    fflush(stdout);
    
    // Create Wayland global for this output
    output->global = wl_global_create(compositor->display,
                                     &wl_output_interface, 3,
                                     output, output_bind);
    
    if (!output->global) {
        fprintf(stderr, "      [OUTPUT_CREATE] Failed to create Wayland global\n");
        gbm_surface_destroy(output->gbm_surface);
        free(output);
        return NULL;
    }
    
    printf("      [OUTPUT_CREATE] Adding to output list...\n");
    fflush(stdout);
    
    wl_list_insert(&compositor->output_list, &output->link);
    
    printf("      [OUTPUT_CREATE] Output created successfully!\n");
    printf("      [OUTPUT_CREATE] Triggering initial repaint to set mode...\n");
    fflush(stdout);
    
    // Trigger initial repaint to set the display mode and show something
    triangles_output_repaint(output);
    
    return output;
}

void triangles_output_destroy(struct triangles_output *output) {
    if (!output) return;
    
    // Restore original CRTC
    if (output->saved_crtc) {
        drmModeSetCrtc(output->compositor->drm_fd,
                      output->saved_crtc->crtc_id,
                      output->saved_crtc->buffer_id,
                      output->saved_crtc->x,
                      output->saved_crtc->y,
                      &output->connector_id, 1,
                      &output->saved_crtc->mode);
        drmModeFreeCrtc(output->saved_crtc);
    }
    
    // Cleanup framebuffer
    if (output->fb_id) {
        drmModeRmFB(output->compositor->drm_fd, output->fb_id);
    }
    
    // Cleanup EGL surface
    if (output->egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(output->compositor->egl_display, output->egl_surface);
    }

    // Cleanup GBM
    if (output->current_bo) {
        gbm_surface_release_buffer(output->gbm_surface, output->current_bo);
    }
    if (output->next_bo) {
        gbm_surface_release_buffer(output->gbm_surface, output->next_bo);
    }
    if (output->gbm_surface) {
        gbm_surface_destroy(output->gbm_surface);
    }
    
    // Cleanup Wayland global
    if (output->global) {
        wl_global_destroy(output->global);
    }
    
    wl_list_remove(&output->link);
    free(output);
}

void triangles_output_set_scale(struct triangles_output *output, double scale) {
    if (scale <= 0.0 || scale > 4.0) {
        fprintf(stderr, "Invalid scale: %.2f\n", scale);
        return;
    }
    
    output->scale = scale;
    output->scale_int = (int32_t)(scale + 0.5);
    
    printf("Output scale set to %.2f (integer: %d)\n", scale, output->scale_int);
    
    // Trigger repaint with new scale
    triangles_output_repaint(output);
}

// Schedule a repaint for the next event loop iteration.
// Fast — just sets a flag. The main loop drains these after dispatch.
// Called from the main loop's DRM fd event source to service flip callbacks.
void triangles_output_drm_dispatch(struct triangles_compositor *compositor) {
    drmEventContext evctx = {
        .version            = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler  = output_handle_drm_flip,
    };
    drmHandleEvent(compositor->drm_fd, &evctx);
}

void triangles_output_schedule_repaint(struct triangles_output *output) {
    output->needs_repaint = true;
}

void triangles_output_repaint(struct triangles_output *output) {
    struct triangles_compositor *compositor = output->compositor;

    // Don't queue a second flip while one is already in flight —
    // mark dirty so the flip callback will repaint when it lands.
    if (output->flip_pending) {
        output->needs_repaint = true;
        return;
    }

    if (output->egl_surface == EGL_NO_SURFACE) return;

    if (!eglMakeCurrent(compositor->egl_display,
                        output->egl_surface, output->egl_surface,
                        compositor->egl_context)) {
        fprintf(stderr, "[REPAINT] eglMakeCurrent failed: 0x%x\n", eglGetError());
        return;
    }

    triangles_renderer_begin(output);

    struct triangles_view *view;
    wl_list_for_each(view, &compositor->view_list, link) {
        if (view->mapped && view->output == output) {
            if (!view->has_csd)
                triangles_renderer_render_titlebar(view);
            triangles_renderer_render_view(view);
        }
    }

    if (!wl_list_empty(&compositor->seat_list)) {
        struct triangles_seat *seat = wl_container_of(
            compositor->seat_list.next, seat, link);
        triangles_renderer_render_cursor(seat, output);
    }

    triangles_renderer_end(output);

    if (!eglSwapBuffers(compositor->egl_display, output->egl_surface)) {
        fprintf(stderr, "[REPAINT] eglSwapBuffers failed: 0x%x\n", eglGetError());
        return;
    }

    struct gbm_bo *bo = gbm_surface_lock_front_buffer(output->gbm_surface);
    if (!bo) {
        fprintf(stderr, "[REPAINT] gbm_surface_lock_front_buffer failed\n");
        return;
    }

    uint32_t fb_id = get_fb_for_bo(compositor->drm_fd, bo);
    if (!fb_id) {
        gbm_surface_release_buffer(output->gbm_surface, bo);
        return;
    }

    // First frame: use SetCrtc to establish the mode, then switch to PageFlip.
    if (!output->current_bo) {
        int ret = drmModeSetCrtc(compositor->drm_fd, output->crtc_id, fb_id,
                                 0, 0, &output->connector_id, 1, &output->mode);
        if (ret) {
            fprintf(stderr, "[REPAINT] drmModeSetCrtc failed: %d\n", ret);
            gbm_surface_release_buffer(output->gbm_surface, bo);
            return;
        }
        // On the very first frame there's no previous BO to release yet
        output->current_bo = bo;
        output->fb_id      = fb_id;
        return;
    }

    // All subsequent frames: non-blocking page flip
    int ret = drmModePageFlip(compositor->drm_fd, output->crtc_id, fb_id,
                               DRM_MODE_PAGE_FLIP_EVENT, output);
    if (ret) {
        fprintf(stderr, "[REPAINT] drmModePageFlip failed: %d\n", ret);
        gbm_surface_release_buffer(output->gbm_surface, bo);
        return;
    }

    output->next_bo      = bo;
    output->next_fb_id   = fb_id;
    output->flip_pending = true;
}

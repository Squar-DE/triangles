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

static void output_handle_drm_flip(int fd, unsigned int sequence,
                                  unsigned int tv_sec, unsigned int tv_usec,
                                  void *user_data) {
    struct triangles_output *output = user_data;
    
    // Release old buffer
    if (output->current_bo) {
        gbm_surface_release_buffer(output->gbm_surface, output->current_bo);
    }
    
    output->current_bo = output->next_bo;
    output->next_bo = NULL;
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

void triangles_output_repaint(struct triangles_output *output) {
    struct triangles_compositor *compositor = output->compositor;
    
    printf("[REPAINT] Starting repaint for output %dx%d\n", output->width, output->height);
    fflush(stdout);
    
    // Create EGL surface for this output
    EGLSurface egl_surface = eglCreateWindowSurface(compositor->egl_display,
                                                    compositor->egl_config,
                                                    (EGLNativeWindowType)output->gbm_surface,
                                                    NULL);
    if (egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "[REPAINT] Failed to create EGL surface: 0x%x\n", eglGetError());
        return;
    }
    
    // Make current
    if (!eglMakeCurrent(compositor->egl_display, egl_surface, egl_surface,
                       compositor->egl_context)) {
        fprintf(stderr, "[REPAINT] Failed to make EGL context current: 0x%x\n", eglGetError());
        eglDestroySurface(compositor->egl_display, egl_surface);
        return;
    }
    
    // Begin rendering frame
    triangles_renderer_begin(output);
    
    // Count and render all mapped views on this output
    int view_count = 0;
    struct triangles_view *view;
    wl_list_for_each(view, &compositor->view_list, link) {
        if (view->mapped && view->output == output) {
            printf("[REPAINT] Rendering view at (%d, %d) size %dx%d, texture=%u\n",
                   view->x, view->y, view->width, view->height, 
                   view->surface ? view->surface->texture : 0);
            fflush(stdout);
            triangles_renderer_render_view(view);
            view_count++;
        }
    }
    
    printf("[REPAINT] Rendered %d views\n", view_count);
    fflush(stdout);
    
    // End rendering
    triangles_renderer_end(output);
    
    // Swap buffers
    if (!eglSwapBuffers(compositor->egl_display, egl_surface)) {
        fprintf(stderr, "[REPAINT] Failed to swap buffers: 0x%x\n", eglGetError());
        eglDestroySurface(compositor->egl_display, egl_surface);
        return;
    }
    
    printf("[REPAINT] Buffers swapped\n");
    fflush(stdout);
    
    // Get buffer and create framebuffer
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(output->gbm_surface);
    if (!bo) {
        fprintf(stderr, "[REPAINT] Failed to lock front buffer\n");
        eglDestroySurface(compositor->egl_display, egl_surface);
        return;
    }
    
    uint32_t fb_id;
    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    
    int ret = drmModeAddFB(compositor->drm_fd, width, height,
                          24, 32, stride, handle, &fb_id);
    if (ret) {
        fprintf(stderr, "[REPAINT] Failed to create framebuffer: %d\n", ret);
        gbm_surface_release_buffer(output->gbm_surface, bo);
        eglDestroySurface(compositor->egl_display, egl_surface);
        return;
    }
    
    printf("[REPAINT] Created framebuffer %u\n", fb_id);
    fflush(stdout);
    
    // Set CRTC for scanout
    ret = drmModeSetCrtc(compositor->drm_fd, output->crtc_id, fb_id,
                        0, 0, &output->connector_id, 1, &output->mode);
    if (ret) {
        fprintf(stderr, "[REPAINT] Failed to set CRTC: %d\n", ret);
        drmModeRmFB(compositor->drm_fd, fb_id);
        gbm_surface_release_buffer(output->gbm_surface, bo);
        eglDestroySurface(compositor->egl_display, egl_surface);
        return;
    }
    
    printf("[REPAINT] CRTC set successfully - frame displayed!\n");
    fflush(stdout);
    
    // Clean up old framebuffer and buffer
    if (output->fb_id) {
        drmModeRmFB(compositor->drm_fd, output->fb_id);
    }
    if (output->current_bo) {
        gbm_surface_release_buffer(output->gbm_surface, output->current_bo);
    }
    
    output->fb_id = fb_id;
    output->current_bo = output->next_bo;
    output->next_bo = bo;
    
    eglDestroySurface(compositor->egl_display, egl_surface);
}

// dmabuf.c - linux-dmabuf-unstable-v1 protocol implementation
// Implements zwp_linux_dmabuf_v1 for zero-copy GPU buffer sharing
#include "compositor.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <GLES2/gl2ext.h>

// ─── EGL extension function pointers ─────────────────────────────────────────
// These are resolved once during init and used throughout
static PFNEGLCREATEIMAGEKHRPROC            egl_create_image           = NULL;
static PFNEGLDESTROYIMAGEKHRPROC           egl_destroy_image          = NULL;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC egl_image_target_texture2d = NULL;
static PFNEGLQUERYDMABUFFORMATSEXTPROC     egl_query_dmabuf_formats   = NULL;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC   egl_query_dmabuf_modifiers = NULL;

// ─── Pending DMA-BUF params ───────────────────────────────────────────────────
// Accumulates planes before the client calls create/create_immed
#define DMABUF_MAX_PLANES 4

struct triangles_dmabuf_plane {
    int      fd;
    uint32_t offset;
    uint32_t stride;
    uint64_t modifier;
};

struct triangles_dmabuf_params {
    struct triangles_compositor   *compositor;
    struct wl_resource            *resource;

    uint32_t                       width;
    uint32_t                       height;
    uint32_t                       format;   // DRM fourcc
    uint32_t                       flags;

    struct triangles_dmabuf_plane  planes[DMABUF_MAX_PLANES];
    int                            n_planes;
    bool                           has_modifier;
};

// ─── Utility ─────────────────────────────────────────────────────────────────

// Check whether the EGL display supports a given extension string
static bool egl_has_extension(EGLDisplay dpy, const char *name) {
    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!exts) return false;
    size_t len = strlen(name);
    const char *p = exts;
    while ((p = strstr(p, name))) {
        if ((p == exts || p[-1] == ' ') && (p[len] == '\0' || p[len] == ' '))
            return true;
        p += len;
    }
    return false;
}

// ─── EGLImage → GL texture ───────────────────────────────────────────────────

EGLImageKHR triangles_dmabuf_import_eglimage(struct triangles_compositor *compositor,
                                              struct triangles_dmabuf_params *params) {
    if (!egl_create_image) {
        fprintf(stderr, "[DMABUF] eglCreateImageKHR not available\n");
        return EGL_NO_IMAGE_KHR;
    }

    // Build the EGL attrib list.
    // The spec allows up to 4 planes; each plane needs fd, offset, stride
    // and optionally hi/lo modifier words.
    EGLint attribs[64];
    int i = 0;

#define PUSH(k, v) do { attribs[i++] = (k); attribs[i++] = (v); } while (0)

    PUSH(EGL_WIDTH,  (EGLint)params->width);
    PUSH(EGL_HEIGHT, (EGLint)params->height);
    PUSH(EGL_LINUX_DRM_FOURCC_EXT, (EGLint)params->format);

    // Plane 0 is always present
    PUSH(EGL_DMA_BUF_PLANE0_FD_EXT,     params->planes[0].fd);
    PUSH(EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)params->planes[0].offset);
    PUSH(EGL_DMA_BUF_PLANE0_PITCH_EXT,  (EGLint)params->planes[0].stride);
    if (params->has_modifier) {
        PUSH(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
             (EGLint)(params->planes[0].modifier >> 32));
        PUSH(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
             (EGLint)(params->planes[0].modifier & 0xFFFFFFFF));
    }

    if (params->n_planes >= 2) {
        PUSH(EGL_DMA_BUF_PLANE1_FD_EXT,     params->planes[1].fd);
        PUSH(EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)params->planes[1].offset);
        PUSH(EGL_DMA_BUF_PLANE1_PITCH_EXT,  (EGLint)params->planes[1].stride);
        if (params->has_modifier) {
            PUSH(EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                 (EGLint)(params->planes[1].modifier >> 32));
            PUSH(EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                 (EGLint)(params->planes[1].modifier & 0xFFFFFFFF));
        }
    }

    if (params->n_planes >= 3) {
        PUSH(EGL_DMA_BUF_PLANE2_FD_EXT,     params->planes[2].fd);
        PUSH(EGL_DMA_BUF_PLANE2_OFFSET_EXT, (EGLint)params->planes[2].offset);
        PUSH(EGL_DMA_BUF_PLANE2_PITCH_EXT,  (EGLint)params->planes[2].stride);
        if (params->has_modifier) {
            PUSH(EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
                 (EGLint)(params->planes[2].modifier >> 32));
            PUSH(EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
                 (EGLint)(params->planes[2].modifier & 0xFFFFFFFF));
        }
    }

    if (params->n_planes >= 4) {
        PUSH(EGL_DMA_BUF_PLANE3_FD_EXT,     params->planes[3].fd);
        PUSH(EGL_DMA_BUF_PLANE3_OFFSET_EXT, (EGLint)params->planes[3].offset);
        PUSH(EGL_DMA_BUF_PLANE3_PITCH_EXT,  (EGLint)params->planes[3].stride);
        if (params->has_modifier) {
            PUSH(EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
                 (EGLint)(params->planes[3].modifier >> 32));
            PUSH(EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
                 (EGLint)(params->planes[3].modifier & 0xFFFFFFFF));
        }
    }

#undef PUSH
    attribs[i] = EGL_NONE;

    EGLImageKHR image = egl_create_image(compositor->egl_display,
                                         EGL_NO_CONTEXT,
                                         EGL_LINUX_DMA_BUF_EXT,
                                         NULL,
                                         attribs);
    if (image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "[DMABUF] eglCreateImageKHR failed: 0x%x\n", eglGetError());
        return EGL_NO_IMAGE_KHR;
    }

    printf("[DMABUF] Imported EGLImage %p (%dx%d fourcc=0x%08x planes=%d)\n",
           (void *)image, params->width, params->height,
           params->format, params->n_planes);
    return image;
}

// Import a DMA-BUF EGLImage into a GL texture and return the texture ID
GLuint triangles_dmabuf_create_texture(struct triangles_compositor *compositor,
                                        EGLImageKHR image) {
    (void)compositor;  // unused — EGLImage is already bound to the display
    if (!egl_image_target_texture2d) {
        fprintf(stderr, "[DMABUF] glEGLImageTargetTexture2DOES not available\n");
        return 0;
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    egl_image_target_texture2d(GL_TEXTURE_2D, (GLeglImageOES)image);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "[DMABUF] glEGLImageTargetTexture2DOES error: 0x%x\n", err);
        glDeleteTextures(1, &tex);
        return 0;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    printf("[DMABUF] Created GL texture %u from EGLImage\n", tex);
    return tex;
}

// ─── zwp_linux_buffer_params_v1 interface ────────────────────────────────────

static void params_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void params_add(struct wl_client *client,
                        struct wl_resource *resource,
                        int32_t fd,
                        uint32_t plane_idx,
                        uint32_t offset,
                        uint32_t stride,
                        uint32_t modifier_hi,
                        uint32_t modifier_lo) {
    (void)client;
    struct triangles_dmabuf_params *params = wl_resource_get_user_data(resource);

    if (plane_idx >= DMABUF_MAX_PLANES) {
        wl_resource_post_error(resource,
                               ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "plane index %u out of range", plane_idx);
        close(fd);
        return;
    }

    if (params->planes[plane_idx].fd != -1) {
        wl_resource_post_error(resource,
                               ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "plane %u already set", plane_idx);
        close(fd);
        return;
    }

    uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    params->planes[plane_idx].fd       = fd;
    params->planes[plane_idx].offset   = offset;
    params->planes[plane_idx].stride   = stride;
    params->planes[plane_idx].modifier = modifier;

    if (modifier != DRM_FORMAT_MOD_INVALID)
        params->has_modifier = true;

    // Track highest plane index seen
    if ((int)plane_idx >= params->n_planes)
        params->n_planes = (int)plane_idx + 1;

    printf("[DMABUF] params_add: plane=%u fd=%d offset=%u stride=%u mod=0x%016llx\n",
           plane_idx, fd, offset, stride, (unsigned long long)modifier);
}

// Minimal wl_buffer implementation for DMA-BUF buffers
static void dmabuf_wl_buffer_destroy(struct wl_client *client,
                                      struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface dmabuf_wl_buffer_impl = {
    .destroy = dmabuf_wl_buffer_destroy,
};

// Shared logic: import buffer and emit created/failed event.
// buffer_id == 0  → non-immediate (create): server allocates the wl_buffer ID,
//                   then sends zwp_linux_buffer_params_v1.created.
// buffer_id != 0  → immediate (create_immed): client chose the ID upfront,
//                   no event is sent.
static void params_do_create(struct wl_client *client,
                              struct wl_resource *params_resource,
                              uint32_t buffer_id,
                              int32_t  width,
                              int32_t  height,
                              uint32_t format,
                              uint32_t flags) {
    struct triangles_dmabuf_params *params = wl_resource_get_user_data(params_resource);
    struct triangles_compositor *compositor = params->compositor;
    bool immediate = (buffer_id != 0);

    if (width <= 0 || height <= 0) {
        wl_resource_post_error(params_resource,
                               ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
                               "invalid dimensions %dx%d", width, height);
        return;
    }

    params->width  = (uint32_t)width;
    params->height = (uint32_t)height;
    params->format = format;
    params->flags  = flags;

    // Import into EGL
    EGLImageKHR image = triangles_dmabuf_import_eglimage(compositor, params);
    if (image == EGL_NO_IMAGE_KHR) {
        if (!immediate) {
            // Non-immediate: protocol allows sending "failed" event
            zwp_linux_buffer_params_v1_send_failed(params_resource);
        } else {
            // Immediate: must post a fatal protocol error instead
            wl_resource_post_error(params_resource,
                                   ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                                   "DMA-BUF import failed");
        }
        return;
    }

    // Allocate our wl_buffer user-data wrapper
    struct triangles_dmabuf_buffer *buf = calloc(1, sizeof(*buf));
    if (!buf) {
        egl_destroy_image(compositor->egl_display, image);
        wl_client_post_no_memory(client);
        return;
    }

    buf->compositor = compositor;
    buf->image      = image;
    buf->width      = params->width;
    buf->height     = params->height;
    buf->format     = format;

    // Close all plane FDs — EGL has dup'd them internally
    for (int p = 0; p < params->n_planes; p++) {
        if (params->planes[p].fd != -1) {
            close(params->planes[p].fd);
            params->planes[p].fd = -1;
        }
    }

    // Create the wl_buffer resource.
    // ID=0 → libwayland picks a server-side ID (non-immediate path).
    // ID=buffer_id → use the client-chosen ID (create_immed path).
    struct wl_resource *buf_resource = wl_resource_create(client,
                                                           &wl_buffer_interface,
                                                           1,
                                                           buffer_id);
    if (!buf_resource) {
        egl_destroy_image(compositor->egl_display, image);
        free(buf);
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(buf_resource, &dmabuf_wl_buffer_impl,
                                   buf, triangles_dmabuf_buffer_destroy);

    if (!immediate) {
        // Send the "created" event so the client learns the server-allocated ID
        zwp_linux_buffer_params_v1_send_created(params_resource, buf_resource);
    }
    // Immediate path: client already knows the ID, no event needed
}

static void params_create(struct wl_client *client,
                           struct wl_resource *resource,
                           int32_t  width,
                           int32_t  height,
                           uint32_t format,
                           uint32_t flags) {
    // Non-immediate path: buffer_id=0 signals server-side allocation.
    // wl_resource_create(client, iface, version, 0) lets libwayland pick a
    // fresh server-side object ID.  We then send that resource in the
    // "created" event so the client learns the ID.
    params_do_create(client, resource, 0, width, height, format, flags);
}

static void params_create_immed(struct wl_client *client,
                                 struct wl_resource *resource,
                                 uint32_t buffer_id,
                                 int32_t  width,
                                 int32_t  height,
                                 uint32_t format,
                                 uint32_t flags) {
    params_do_create(client, resource, buffer_id, width, height, format, flags);
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy       = params_destroy,
    .add           = params_add,
    .create        = params_create,
    .create_immed  = params_create_immed,
};

static void params_resource_destroy(struct wl_resource *resource) {
    struct triangles_dmabuf_params *params = wl_resource_get_user_data(resource);
    // Close any FDs that were never consumed
    for (int i = 0; i < DMABUF_MAX_PLANES; i++) {
        if (params->planes[i].fd != -1)
            close(params->planes[i].fd);
    }
    free(params);
}

// ─── zwp_linux_dmabuf_v1 interface ───────────────────────────────────────────

static void dmabuf_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void dmabuf_create_params(struct wl_client *client,
                                  struct wl_resource *resource,
                                  uint32_t id) {
    struct triangles_compositor *compositor = wl_resource_get_user_data(resource);

    struct triangles_dmabuf_params *params = calloc(1, sizeof(*params));
    if (!params) {
        wl_client_post_no_memory(client);
        return;
    }

    params->compositor = compositor;
    // Pre-fill FDs with -1 so we can detect unset planes
    for (int i = 0; i < DMABUF_MAX_PLANES; i++)
        params->planes[i].fd = -1;

    uint32_t version = wl_resource_get_version(resource);
    params->resource = wl_resource_create(client,
                                          &zwp_linux_buffer_params_v1_interface,
                                          version, id);
    if (!params->resource) {
        free(params);
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(params->resource, &params_impl,
                                   params, params_resource_destroy);
}

// v4 surface feedback (stub — keeps v4 clients from erroring)
static void dmabuf_get_default_feedback(struct wl_client *client,
                                         struct wl_resource *resource,
                                         uint32_t id) {
    // TODO: Implement proper surface feedback with tranche format/modifier tables
    // For now, create a placeholder resource and immediately send "done"
    struct wl_resource *fb = wl_resource_create(client,
        &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
    if (!fb) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(fb, NULL, NULL, NULL);
    zwp_linux_dmabuf_feedback_v1_send_done(fb);
}

static void dmabuf_get_surface_feedback(struct wl_client *client,
                                          struct wl_resource *resource,
                                          uint32_t id,
                                          struct wl_resource *surface) {
    dmabuf_get_default_feedback(client, resource, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy                = dmabuf_destroy,
    .create_params          = dmabuf_create_params,
    .get_default_feedback   = dmabuf_get_default_feedback,
    .get_surface_feedback   = dmabuf_get_surface_feedback,
};

// ─── Advertise supported formats ─────────────────────────────────────────────

// Common DRM formats always advertised even without driver query
static const uint32_t fallback_formats[] = {
    DRM_FORMAT_ARGB8888,
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ABGR8888,
    DRM_FORMAT_XBGR8888,
    DRM_FORMAT_RGB888,
    DRM_FORMAT_BGR888,
    DRM_FORMAT_ABGR2101010,
    DRM_FORMAT_XBGR2101010,
    DRM_FORMAT_ARGB2101010,
    DRM_FORMAT_XRGB2101010,
    DRM_FORMAT_YUYV,
    DRM_FORMAT_NV12,
    DRM_FORMAT_YUV420,
};

static void send_formats_to_client(struct wl_resource *resource,
                                    struct triangles_compositor *compositor) {
    uint32_t version = wl_resource_get_version(resource);

    if (egl_query_dmabuf_formats && egl_query_dmabuf_modifiers) {
        // Query driver-supported formats
        EGLint count = 0;
        if (!egl_query_dmabuf_formats(compositor->egl_display, 0, NULL, &count)
            || count <= 0) {
            goto fallback;
        }

        EGLint *formats = calloc((size_t)count, sizeof(EGLint));
        if (!formats) goto fallback;

        if (!egl_query_dmabuf_formats(compositor->egl_display, count, formats, &count)) {
            free(formats);
            goto fallback;
        }

        printf("[DMABUF] Driver advertises %d DMA-BUF formats\n", count);

        for (int f = 0; f < count; f++) {
            uint32_t fmt = (uint32_t)formats[f];

            // Query modifiers for this format
            EGLint mod_count = 0;
            egl_query_dmabuf_modifiers(compositor->egl_display,
                                       (EGLint)fmt, 0, NULL, NULL, &mod_count);

            if (mod_count > 0) {
                EGLuint64KHR *mods    = calloc((size_t)mod_count, sizeof(EGLuint64KHR));
                EGLBoolean   *extern_ = calloc((size_t)mod_count, sizeof(EGLBoolean));

                if (mods && extern_) {
                    egl_query_dmabuf_modifiers(compositor->egl_display,
                                               (EGLint)fmt,
                                               mod_count, mods, extern_,
                                               &mod_count);
                    for (int m = 0; m < mod_count; m++) {
                        if (version >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
                            zwp_linux_dmabuf_v1_send_modifier(resource, fmt,
                                (uint32_t)(mods[m] >> 32),
                                (uint32_t)(mods[m] & 0xFFFFFFFF));
                        } else {
                            zwp_linux_dmabuf_v1_send_format(resource, fmt);
                        }
                    }
                }
                free(mods);
                free(extern_);
            } else {
                // No modifiers — advertise LINEAR
                if (version >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
                    zwp_linux_dmabuf_v1_send_modifier(resource, fmt,
                        (uint32_t)(DRM_FORMAT_MOD_LINEAR >> 32),
                        (uint32_t)(DRM_FORMAT_MOD_LINEAR & 0xFFFFFFFF));
                } else {
                    zwp_linux_dmabuf_v1_send_format(resource, fmt);
                }
            }
        }
        free(formats);
        return;
    }

fallback:
    printf("[DMABUF] Using fallback format list\n");
    for (size_t i = 0; i < sizeof(fallback_formats)/sizeof(fallback_formats[0]); i++) {
        if (version >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
            zwp_linux_dmabuf_v1_send_modifier(resource,
                fallback_formats[i],
                (uint32_t)(DRM_FORMAT_MOD_LINEAR >> 32),
                (uint32_t)(DRM_FORMAT_MOD_LINEAR & 0xFFFFFFFF));
        } else {
            zwp_linux_dmabuf_v1_send_format(resource, fallback_formats[i]);
        }
    }
}

// ─── Global bind ─────────────────────────────────────────────────────────────

static void dmabuf_bind(struct wl_client *client, void *data,
                         uint32_t version, uint32_t id) {
    struct triangles_compositor *compositor = data;

    struct wl_resource *resource = wl_resource_create(client,
                                                       &zwp_linux_dmabuf_v1_interface,
                                                       version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(resource, &dmabuf_impl, compositor, NULL);

    // Advertise formats/modifiers to this client immediately on bind
    // (versions 1–3 use format/modifier events; version 4 uses feedback objects)
    if (version < 4) {
        send_formats_to_client(resource, compositor);
    }
}

// ─── wl_buffer destroy callback ──────────────────────────────────────────────

void triangles_dmabuf_buffer_destroy(struct wl_resource *resource) {
    struct triangles_dmabuf_buffer *buf = wl_resource_get_user_data(resource);
    if (!buf) return;

    if (buf->image != EGL_NO_IMAGE_KHR && egl_destroy_image) {
        egl_destroy_image(buf->compositor->egl_display, buf->image);
        buf->image = EGL_NO_IMAGE_KHR;
    }
    free(buf);
}

// ─── renderer integration ────────────────────────────────────────────────────

// Called from renderer.c when a non-SHM buffer is committed to a surface
GLuint triangles_dmabuf_import_texture(struct triangles_compositor *compositor,
                                        struct wl_resource *buffer) {
    // Retrieve our wrapper stored as the buffer's user-data
    struct triangles_dmabuf_buffer *buf = wl_resource_get_user_data(buffer);
    if (!buf || buf->image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "[DMABUF] No EGLImage in buffer resource\n");
        return 0;
    }
    return triangles_dmabuf_create_texture(compositor, buf->image);
}

// ─── Public init ─────────────────────────────────────────────────────────────

bool triangles_dmabuf_init(struct triangles_compositor *compositor) {
    EGLDisplay dpy = compositor->egl_display;

    // Require core DMA-BUF import support
    if (!egl_has_extension(dpy, "EGL_EXT_image_dma_buf_import")) {
        fprintf(stderr, "[DMABUF] EGL_EXT_image_dma_buf_import not supported — "
                        "DMA-BUF disabled\n");
        return false;
    }

    // Resolve mandatory functions
    egl_create_image = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    egl_destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    egl_image_target_texture2d = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!egl_create_image || !egl_destroy_image || !egl_image_target_texture2d) {
        fprintf(stderr, "[DMABUF] Failed to resolve required EGL/GL extensions\n");
        return false;
    }

    // Resolve optional modifier query (v3 feature)
    if (egl_has_extension(dpy, "EGL_EXT_image_dma_buf_import_modifiers")) {
        egl_query_dmabuf_formats = (PFNEGLQUERYDMABUFFORMATSEXTPROC)
            eglGetProcAddress("eglQueryDmaBufFormatsEXT");
        egl_query_dmabuf_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)
            eglGetProcAddress("eglQueryDmaBufModifiersEXT");
        printf("[DMABUF] Modifier support available\n");
    } else {
        printf("[DMABUF] No modifier support — LINEAR assumed\n");
    }

    // Advertise zwp_linux_dmabuf_v1 — support up to version 4
    // (version 4 adds feedback objects; earlier versions use format events)
    compositor->dmabuf_global = wl_global_create(compositor->display,
                                                  &zwp_linux_dmabuf_v1_interface,
                                                  4,
                                                  compositor,
                                                  dmabuf_bind);
    if (!compositor->dmabuf_global) {
        fprintf(stderr, "[DMABUF] Failed to create zwp_linux_dmabuf_v1 global\n");
        return false;
    }

    printf("[DMABUF] linux-dmabuf-unstable-v1 initialized (v4)\n");
    return true;
}

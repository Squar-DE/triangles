// compositor.c - Core compositor implementation
#include "compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

struct triangles_compositor *triangles_compositor_create(void) {
    struct triangles_compositor *compositor = calloc(1, sizeof(*compositor));
    if (!compositor) {
        return NULL;
    }
    
    compositor->display = wl_display_create();
    if (!compositor->display) {
        free(compositor);
        return NULL;
    }
    
    compositor->event_loop = wl_display_get_event_loop(compositor->display);
    
    wl_list_init(&compositor->output_list);
    wl_list_init(&compositor->surface_list);
    wl_list_init(&compositor->view_list);
    wl_list_init(&compositor->seat_list);
    
    compositor->running = true;
    compositor->fractional_scale_enabled = true;
    compositor->default_scale = 1.0;
    
    return compositor;
}

static bool init_drm(struct triangles_compositor *compositor) {
    // Open DRM device - try common card paths
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        
        compositor->drm_fd = open(path, O_RDWR | O_CLOEXEC);
        if (compositor->drm_fd >= 0) {
            printf("Opened DRM device: %s\n", path);
            break;
        }
    }
    
    if (compositor->drm_fd < 0) {
        fprintf(stderr, "Failed to open DRM device\n");
        return false;
    }
    
    // Get DRM resources
    compositor->resources = drmModeGetResources(compositor->drm_fd);
    if (!compositor->resources) {
        fprintf(stderr, "Failed to get DRM resources\n");
        close(compositor->drm_fd);
        return false;
    }
    
    // Initialize GBM
    compositor->gbm = gbm_create_device(compositor->drm_fd);
    if (!compositor->gbm) {
        fprintf(stderr, "Failed to create GBM device\n");
        drmModeFreeResources(compositor->resources);
        close(compositor->drm_fd);
        return false;
    }
    
    return true;
}

static bool init_egl(struct triangles_compositor *compositor) {
    // Get EGL display from GBM using platform extension
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    
    if (get_platform_display) {
        compositor->egl_display = get_platform_display(EGL_PLATFORM_GBM_KHR,
                                                       compositor->gbm, NULL);
    } else {
        compositor->egl_display = eglGetDisplay((EGLNativeDisplayType)compositor->gbm);
    }
    
    if (compositor->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return false;
    }    

    // Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(compositor->egl_display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return false;
    }
    
    printf("EGL version: %d.%d\n", major, minor);
    
    // Bind OpenGL ES API
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "Failed to bind OpenGL ES API\n");
        return false;
    }
    
    // Choose EGL config
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    
    EGLint num_configs;
    if (!eglChooseConfig(compositor->egl_display, config_attribs,
                         &compositor->egl_config, 1, &num_configs) || num_configs == 0) {
        fprintf(stderr, "Failed to choose EGL config\n");
        return false;
    }
    
    // Create EGL context
    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    
    compositor->egl_context = eglCreateContext(compositor->egl_display,
                                               compositor->egl_config,
                                               EGL_NO_CONTEXT,
                                               context_attribs);
    if (compositor->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        return false;
    }
    
    return true;
}

static bool init_outputs(struct triangles_compositor *compositor) {
    printf("    [OUTPUT] Scanning for connected displays...\n");
    printf("    [OUTPUT] Found %d connectors\n", compositor->resources->count_connectors);
    fflush(stdout);
    
    // Find and initialize all connected outputs
    for (int i = 0; i < compositor->resources->count_connectors; i++) {
        uint32_t connector_id = compositor->resources->connectors[i];
        printf("    [OUTPUT] Checking connector %d (id=%u)...\n", i, connector_id);
        fflush(stdout);
        
        drmModeConnector *connector = drmModeGetConnector(compositor->drm_fd, connector_id);
        
        if (!connector) {
            printf("    [OUTPUT] Failed to get connector %d\n", i);
            continue;
        }
        
        printf("    [OUTPUT] Connector %d: connection=%d, modes=%d\n", 
               i, connector->connection, connector->count_modes);
        fflush(stdout);
        
        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            printf("    [OUTPUT] Creating output for connector %d...\n", i);
            fflush(stdout);
            
            struct triangles_output *output = triangles_output_create(compositor, connector_id);
            if (output) {
                printf("    [OUTPUT] ✓ Initialized output: %dx%d@%.2fHz scale=%.2f\n",
                       output->width, output->height,
                       output->refresh / 1000.0, output->scale);
                fflush(stdout);
            } else {
                printf("    [OUTPUT] ✗ Failed to create output for connector %d\n", i);
                fflush(stdout);
            }
        }
        
        drmModeFreeConnector(connector);
    }
    
    if (wl_list_empty(&compositor->output_list)) {
        fprintf(stderr, "    [OUTPUT] ✗ No outputs found\n");
        return false;
    }
    
    printf("    [OUTPUT] Output initialization complete\n");
    fflush(stdout);
    return true;
}

bool triangles_compositor_init(struct triangles_compositor *compositor) {
    printf("  [INIT] Starting udev...\n");
    fflush(stdout);
    
    // Initialize udev for device management
    compositor->udev = udev_new();
    if (!compositor->udev) {
        fprintf(stderr, "Failed to initialize udev\n");
        return false;
    }
    printf("  [INIT] udev initialized\n");
    fflush(stdout);
    
    // Initialize DRM/KMS
    printf("  [INIT] Initializing DRM/KMS...\n");
    fflush(stdout);
    if (!init_drm(compositor)) {
        fprintf(stderr, "  [INIT] DRM initialization failed\n");
        return false;
    }
    printf("  [INIT] DRM/KMS initialized\n");
    fflush(stdout);
    
    // Initialize EGL
    printf("  [INIT] Initializing EGL...\n");
    fflush(stdout);
    if (!init_egl(compositor)) {
        fprintf(stderr, "  [INIT] EGL initialization failed\n");
        return false;
    }
    printf("  [INIT] EGL initialized\n");
    fflush(stdout);
    
    // Initialize renderer
    printf("  [INIT] Initializing renderer...\n");
    fflush(stdout);
    if (!triangles_renderer_init(compositor)) {
        fprintf(stderr, "  [INIT] Renderer initialization failed\n");
        return false;
    }
    printf("  [INIT] Renderer initialized\n");
    fflush(stdout);
    
    // Initialize outputs
    printf("  [INIT] Initializing outputs...\n");
    fflush(stdout);
    if (!init_outputs(compositor)) {
        fprintf(stderr, "  [INIT] Output initialization failed\n");
        return false;
    }
    printf("  [INIT] Outputs initialized\n");
    fflush(stdout);
    
    // Initialize input
    printf("  [INIT] Initializing input system...\n");
    fflush(stdout);
    if (!triangles_input_init(compositor)) {
        fprintf(stderr, "Warning: Failed to initialize input\n");
        // Non-fatal for now
    }
    printf("  [INIT] Input system initialized\n");
    fflush(stdout);
    
    // Initialize Wayland globals and protocols
    // These will be implemented in separate protocol files
    printf("  [INIT] Initializing protocols...\n");
    fflush(stdout);
    extern bool triangles_compositor_protocol_init(struct triangles_compositor *compositor);
    extern bool triangles_xdg_shell_init(struct triangles_compositor *compositor);
    
    if (!triangles_compositor_protocol_init(compositor)) {
        fprintf(stderr, "  [INIT] Protocol initialization failed\n");
        return false;
    }
    printf("  [INIT] Protocols initialized\n");
    fflush(stdout);
    
    if (!triangles_xdg_shell_init(compositor)) {
        fprintf(stderr, "Warning: Failed to initialize xdg-shell\n");
    }
    printf("  [INIT] XDG shell initialized\n");
    fflush(stdout);
    
    printf("  [INIT] All subsystems ready!\n");
    fflush(stdout);
    return true;
}

void triangles_compositor_destroy(struct triangles_compositor *compositor) {
    if (!compositor) return;
    
    // Destroy outputs
    struct triangles_output *output, *tmp_output;
    wl_list_for_each_safe(output, tmp_output, &compositor->output_list, link) {
        triangles_output_destroy(output);
    }
    
    // Destroy surfaces
    struct triangles_surface *surface, *tmp_surface;
    wl_list_for_each_safe(surface, tmp_surface, &compositor->surface_list, link) {
        triangles_surface_destroy(surface);
    }
    
    // Cleanup EGL
    if (compositor->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(compositor->egl_display, EGL_NO_SURFACE, 
                      EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (compositor->egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(compositor->egl_display, compositor->egl_context);
        }
        eglTerminate(compositor->egl_display);
    }
    
    // Cleanup GBM
    if (compositor->gbm) {
        gbm_device_destroy(compositor->gbm);
    }
    
    // Cleanup DRM
    if (compositor->resources) {
        drmModeFreeResources(compositor->resources);
    }
    if (compositor->drm_fd >= 0) {
        close(compositor->drm_fd);
    }
    
    // Cleanup libinput
    if (compositor->libinput) {
        libinput_unref(compositor->libinput);
    }
    
    // Cleanup udev
    if (compositor->udev) {
        udev_unref(compositor->udev);
    }
    
    // Cleanup Wayland
    if (compositor->display) {
        wl_display_destroy(compositor->display);
    }

    // Cleanup libseat
if (compositor->seat) {
    libseat_close_seat(compositor->seat);
}
    
    free(compositor);
}

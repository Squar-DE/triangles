#include <wayland-server.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <signal.h>

#include "xdg-shell-protocol.h"

// DRM/KMS structures
struct drm_device {
    int fd;
    drmModeRes *resources;
    drmModeConnector *connector;
    drmModeEncoder *encoder;
    drmModeCrtc *crtc;
    drmModeModeInfo *mode;
    uint32_t connector_id;
    uint32_t crtc_id;
};

struct drm_framebuffer {
    uint32_t fb_id;
    uint32_t handle;
    uint32_t stride;
    uint32_t size;
    void *map;
};

// Protocol interfaces
static struct wl_compositor_interface compositor_interface;
static struct wl_shell_interface shell_interface;

struct tiny_compositor {
    struct wl_display *display;
    struct wl_global *compositor_global;
    struct wl_global *shell_global;
    
    // DRM/GBM/EGL
    struct drm_device drm;
    struct gbm_device *gbm_device;
    struct gbm_surface *gbm_surface;
    
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLConfig egl_config;
    EGLSurface egl_surface;
    
    // Frame buffers for double buffering
    struct drm_framebuffer fb[2];
    int current_fb;
    
    int width;
    int height;
    int running;
};

static struct tiny_compositor *g_comp = NULL;

static void cleanup_and_exit(int sig) {
    (void)sig; // Suppress unused parameter warning
    if (g_comp) {
        g_comp->running = 0;
    }
}

// Find a suitable DRM device
static int find_drm_device(struct drm_device *drm) {
    drmDevicePtr devices[64];
    int num_devices, i;
    
    num_devices = drmGetDevices2(0, devices, 64);
    if (num_devices < 0) {
        fprintf(stderr, "Failed to get DRM devices\n");
        return -1;
    }
    
    for (i = 0; i < num_devices; i++) {
        drmDevicePtr device = devices[i];
        
        if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;
            
        drm->fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR);
        if (drm->fd < 0)
            continue;
            
        // Test if this device supports KMS
        if (drmSetClientCap(drm->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0) {
            printf("Using DRM device: %s\n", device->nodes[DRM_NODE_PRIMARY]);
            drmFreeDevices(devices, num_devices);
            return 0;
        }
        
        close(drm->fd);
    }
    
    drmFreeDevices(devices, num_devices);
    fprintf(stderr, "No suitable DRM device found\n");
    return -1;
}

// Initialize DRM and find connected display
static int init_drm(struct drm_device *drm) {
    int i;
    
    if (find_drm_device(drm) < 0) {
        return -1;
    }
    
    drm->resources = drmModeGetResources(drm->fd);
    if (!drm->resources) {
        fprintf(stderr, "Failed to get DRM resources\n");
        return -1;
    }
    
    // Find connected connector
    for (i = 0; i < drm->resources->count_connectors; i++) {
        drm->connector = drmModeGetConnector(drm->fd, drm->resources->connectors[i]);
        if (drm->connector->connection == DRM_MODE_CONNECTED) {
            drm->connector_id = drm->connector->connector_id;
            break;
        }
        drmModeFreeConnector(drm->connector);
        drm->connector = NULL;
    }
    
    if (!drm->connector) {
        fprintf(stderr, "No connected display found\n");
        return -1;
    }
    
    // Use the first mode (usually preferred)
    drm->mode = &drm->connector->modes[0];
    printf("Display mode: %dx%d@%d\n", drm->mode->hdisplay, drm->mode->vdisplay, drm->mode->vrefresh);
    
    // Find encoder
    for (i = 0; i < drm->resources->count_encoders; i++) {
        drm->encoder = drmModeGetEncoder(drm->fd, drm->resources->encoders[i]);
        if (drm->encoder->encoder_id == drm->connector->encoder_id) {
            break;
        }
        drmModeFreeEncoder(drm->encoder);
        drm->encoder = NULL;
    }
    
    if (!drm->encoder) {
        fprintf(stderr, "No encoder found\n");
        return -1;
    }
    
    // Get CRTC
    drm->crtc = drmModeGetCrtc(drm->fd, drm->encoder->crtc_id);
    drm->crtc_id = drm->crtc->crtc_id;
    
    return 0;
}

// Initialize GBM
static int init_gbm(struct tiny_compositor *comp) {
    comp->gbm_device = gbm_create_device(comp->drm.fd);
    if (!comp->gbm_device) {
        fprintf(stderr, "Failed to create GBM device\n");
        return -1;
    }
    
    comp->gbm_surface = gbm_surface_create(comp->gbm_device,
                                          comp->drm.mode->hdisplay,
                                          comp->drm.mode->vdisplay,
                                          GBM_FORMAT_XRGB8888,
                                          GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!comp->gbm_surface) {
        fprintf(stderr, "Failed to create GBM surface\n");
        return -1;
    }
    
    return 0;
}

// Initialize EGL
static int init_egl(struct tiny_compositor *comp) {
    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    
    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    // 1. Get EGL display
    comp->egl_display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, 
                                            (void *)comp->gbm_device, 
                                            NULL);
    if (comp->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display: %s\n", eglGetErrorString());
        return -1;
    }

    // 2. Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(comp->egl_display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL: %s\n", eglGetErrorString());
        return -1;
    }
    printf("EGL %d.%d initialized\n", major, minor);

    // 3. Choose config
    EGLint num_configs;
    if (!eglChooseConfig(comp->egl_display, config_attribs, &comp->egl_config, 1, &num_configs)) {
        fprintf(stderr, "Failed to choose EGL config: %s\n", eglGetErrorString());
        return -1;
    }

    // 4. Create context
    comp->egl_context = eglCreateContext(comp->egl_display, comp->egl_config,
                                       EGL_NO_CONTEXT, context_attribs);
    if (comp->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context: %s\n", eglGetErrorString());
        return -1;
    }

    // 5. Create surface
    comp->egl_surface = eglCreatePlatformWindowSurface(comp->egl_display, 
                                                     comp->egl_config,
                                                     comp->gbm_surface,
                                                     NULL);
    if (comp->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL surface: %s\n", eglGetErrorString());
        return -1;
    }

    // 6. Make current
    if (!eglMakeCurrent(comp->egl_display, comp->egl_surface, comp->egl_surface, comp->egl_context)) {
        fprintf(stderr, "Failed to make EGL context current: %s\n", eglGetErrorString());
        return -1;
    }

    printf("EGL successfully initialized\n");
    return 0;
}


static const char *eglGetErrorString() {
    switch (eglGetError()) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
        default: return "Unknown error";
    }
}


// Create DRM framebuffer from GBM buffer object
static int create_fb_from_bo(struct tiny_compositor *comp, struct gbm_bo *bo, struct drm_framebuffer *fb) {
    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t handle = gbm_bo_get_handle(bo).u32;
    
    int ret = drmModeAddFB(comp->drm.fd, width, height, 24, 32, stride, handle, &fb->fb_id);
    if (ret) {
        fprintf(stderr, "Failed to create framebuffer: %s\n", strerror(errno));
        return -1;
    }
    
    fb->handle = handle;
    fb->stride = stride;
    fb->size = stride * height;
    
    return 0;
}

// Render frame and present to display
static void render_frame(struct tiny_compositor *comp) {
    struct gbm_bo *bo;
    struct drm_framebuffer fb;
    
    // Clear screen with a nice blue color
    glViewport(0, 0, comp->width, comp->height);
    glClearColor(0.1f, 0.2f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Simple OpenGL ES 2.0 rendering - just clear to blue
    // TODO: Add proper shader-based rendering for surfaces
    
    // Swap buffers
    eglSwapBuffers(comp->egl_display, comp->egl_surface);
    
    // Get the buffer object from the surface
    bo = gbm_surface_lock_front_buffer(comp->gbm_surface);
    if (!bo) {
        fprintf(stderr, "Failed to lock front buffer\n");
        return;
    }
    
    // Create framebuffer from buffer object
    if (create_fb_from_bo(comp, bo, &fb) < 0) {
        gbm_surface_release_buffer(comp->gbm_surface, bo);
        return;
    }
    
    // Set the CRTC to display our framebuffer
    int ret = drmModeSetCrtc(comp->drm.fd, comp->drm.crtc_id, fb.fb_id, 0, 0,
                            &comp->drm.connector_id, 1, comp->drm.mode);
    if (ret) {
        fprintf(stderr, "Failed to set CRTC: %s\n", strerror(errno));
    }
    
    // Clean up
    drmModeRmFB(comp->drm.fd, fb.fb_id);
    gbm_surface_release_buffer(comp->gbm_surface, bo);
}

// Wayland compositor interface implementations
static void compositor_create_surface(struct wl_client *client,
                                    struct wl_resource *resource,
                                    uint32_t id) {
    (void)client;   // Suppress unused parameter warning
    (void)resource; // Suppress unused parameter warning
    printf("Client created surface %d\n", id);
    // TODO: Implement surface creation
}

static void compositor_create_region(struct wl_client *client,
                                   struct wl_resource *resource,
                                   uint32_t id) {
    (void)client;   // Suppress unused parameter warning
    (void)resource; // Suppress unused parameter warning
    printf("Client created region %d\n", id);
    // TODO: Implement region creation
}

static struct wl_compositor_interface compositor_interface = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void shell_get_shell_surface(struct wl_client *client,
                                  struct wl_resource *resource,
                                  uint32_t id,
                                  struct wl_resource *surface) {
    (void)client;   // Suppress unused parameter warning
    (void)resource; // Suppress unused parameter warning
    (void)surface;  // Suppress unused parameter warning
    printf("Client requested shell surface %d\n", id);
    // TODO: Implement shell surface creation
}

static struct wl_shell_interface shell_interface = {
    .get_shell_surface = shell_get_shell_surface,
};

static void bind_compositor(struct wl_client *client, void *data,
                          uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(resource, &compositor_interface, data, NULL);
    printf("Compositor bound for client\n");
}

static void bind_shell(struct wl_client *client, void *data,
                      uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &wl_shell_interface, version, id);
    wl_resource_set_implementation(resource, &shell_interface, data, NULL);
    printf("Shell bound for client\n");
}

// Cleanup function
static void cleanup(struct tiny_compositor *comp) {
    if (comp->egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(comp->egl_display, comp->egl_surface);
    }
    if (comp->egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(comp->egl_display, comp->egl_context);
    }
    if (comp->egl_display != EGL_NO_DISPLAY) {
        eglTerminate(comp->egl_display);
    }
    if (comp->gbm_surface) {
        gbm_surface_destroy(comp->gbm_surface);
    }
    if (comp->gbm_device) {
        gbm_device_destroy(comp->gbm_device);
    }
    if (comp->drm.crtc) {
        drmModeFreeCrtc(comp->drm.crtc);
    }
    if (comp->drm.encoder) {
        drmModeFreeEncoder(comp->drm.encoder);
    }
    if (comp->drm.connector) {
        drmModeFreeConnector(comp->drm.connector);
    }
    if (comp->drm.resources) {
        drmModeFreeResources(comp->drm.resources);
    }
    if (comp->drm.fd >= 0) {
        close(comp->drm.fd);
    }
    if (comp->display) {
        wl_display_destroy(comp->display);
    }
}

int main(int argc, char *argv[]) {
    (void)argc; // Suppress unused parameter warning
    (void)argv; // Suppress unused parameter warning
    struct tiny_compositor comp = {0};
    g_comp = &comp;
    comp.running = 1;
    
    // Set up signal handlers
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    
    // Initialize DRM
    if (init_drm(&comp.drm) < 0) {
        fprintf(stderr, "Failed to initialize DRM\n");
        return EXIT_FAILURE;
    }
    
    comp.width = comp.drm.mode->hdisplay;
    comp.height = comp.drm.mode->vdisplay;
    
    // Initialize GBM
    if (init_gbm(&comp) < 0) {
        fprintf(stderr, "Failed to initialize GBM\n");
        cleanup(&comp);
        return EXIT_FAILURE;
    }
    
    // Initialize EGL
    if (init_egl(&comp) < 0) {
        fprintf(stderr, "Failed to initialize EGL\n");
        cleanup(&comp);
        return EXIT_FAILURE;
    }
    
    // Create Wayland display
    comp.display = wl_display_create();
    if (!comp.display) {
        fprintf(stderr, "Failed to create Wayland display\n");
        cleanup(&comp);
        return EXIT_FAILURE;
    }
    
    // Set up global interfaces
    comp.compositor_global = wl_global_create(comp.display,
                                            &wl_compositor_interface,
                                            4, &comp, bind_compositor);
    comp.shell_global = wl_global_create(comp.display,
                                       &wl_shell_interface,
                                       1, &comp, bind_shell);
    
    // Add socket
    const char *socket_name = "wayland-1";
    if (wl_display_add_socket(comp.display, socket_name)) {
        fprintf(stderr, "Failed to add socket '%s' to Wayland display\n", socket_name);
        cleanup(&comp);
        return EXIT_FAILURE;
    }
    
    printf("Running DRM-based Wayland compositor on %s\n", socket_name);
    printf("Display: %dx%d@%dHz\n", comp.width, comp.height, comp.drm.mode->vrefresh);
    printf("OpenGL ES 2.0 renderer: %s\n", glGetString(GL_RENDERER));
    printf("Clients can connect with: WAYLAND_DISPLAY=%s <client>\n", socket_name);
    
    // Main loop
    while (comp.running) {
        wl_display_flush_clients(comp.display);
        render_frame(&comp);
        
        struct wl_event_loop *loop = wl_display_get_event_loop(comp.display);
        wl_event_loop_dispatch(loop, 16); // 16ms timeout (~60fps)
    }
    
    printf("Shutting down compositor...\n");
    cleanup(&comp);
    
    return EXIT_SUCCESS;
}


// Add this to your main.c file to enable proper OpenGL ES 2.0 rendering

// Vertex shader source
static const char *vertex_shader_source = 
    "attribute vec2 position;\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "}\n";

// Fragment shader source
static const char *fragment_shader_source = 
    "precision mediump float;\n"
    "uniform vec3 color;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(color, 1.0);\n"
    "}\n";

// Renderer structure
struct gles2_renderer {
    GLuint program;
    GLuint position_attrib;
    GLuint color_uniform;
    GLuint vbo;
    GLuint vao;
};

// Compile a shader
static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, NULL, log);
        fprintf(stderr, "Shader compilation failed: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    
    return shader;
}

// Create shader program
static GLuint create_program(const char *vertex_source, const char *fragment_source) {
    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    
    if (!vertex_shader || !fragment_shader) {
        return 0;
    }
    
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program, 512, NULL, log);
        fprintf(stderr, "Program linking failed: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    return program;
}

// Initialize renderer
static int init_renderer(struct gles2_renderer *renderer) {
    // Create shader program
    renderer->program = create_program(vertex_shader_source, fragment_shader_source);
    if (!renderer->program) {
        return -1;
    }
    
    // Get attribute and uniform locations
    renderer->position_attrib = glGetAttribLocation(renderer->program, "position");
    renderer->color_uniform = glGetUniformLocation(renderer->program, "color");
    
    // Create vertex buffer for a simple quad
    static const float vertices[] = {
        -0.5f, -0.5f,  // Bottom left
         0.5f, -0.5f,  // Bottom right
         0.5f,  0.5f,  // Top right
        -0.5f,  0.5f   // Top left
    };
    
    glGenBuffers(1, &renderer->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    return 0;
}

// Render a colored quad
static void render_quad(struct gles2_renderer *renderer, float r, float g, float b) {
    glUseProgram(renderer->program);
    
    // Set color uniform
    glUniform3f(renderer->color_uniform, r, g, b);
    
    // Bind vertex buffer
    glBindBuffer(GL_ARRAY_BUFFER, renderer->vbo);
    glEnableVertexAttribArray(renderer->position_attrib);
    glVertexAttribPointer(renderer->position_attrib, 2, GL_FLOAT, GL_FALSE, 0, 0);
    
    // Draw quad as triangle fan
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    
    glDisableVertexAttribArray(renderer->position_attrib);
}

// Cleanup renderer
static void cleanup_renderer(struct gles2_renderer *renderer) {
    if (renderer->vbo) {
        glDeleteBuffers(1, &renderer->vbo);
    }
    if (renderer->program) {
        glDeleteProgram(renderer->program);
    }
}

// Updated render_frame function that uses proper OpenGL ES 2.0
static void render_frame_with_gles2(struct tiny_compositor *comp, struct gles2_renderer *renderer) {
    struct gbm_bo *bo;
    struct drm_framebuffer fb;
    
    // Clear screen with a nice blue color
    glViewport(0, 0, comp->width, comp->height);
    glClearColor(0.1f, 0.2f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Render a red quad using shaders
    render_quad(renderer, 0.8f, 0.2f, 0.2f);
    
    // Swap buffers
    eglSwapBuffers(comp->egl_display, comp->egl_surface);
    
    // Get the buffer object from the surface
    bo = gbm_surface_lock_front_buffer(comp->gbm_surface);
    if (!bo) {
        fprintf(stderr, "Failed to lock front buffer\n");
        return;
    }
    
    // Create framebuffer from buffer object
    if (create_fb_from_bo(comp, bo, &fb) < 0) {
        gbm_surface_release_buffer(comp->gbm_surface, bo);
        return;
    }
    
    // Set the CRTC to display our framebuffer
    int ret = drmModeSetCrtc(comp->drm.fd, comp->drm.crtc_id, fb.fb_id, 0, 0,
                            &comp->drm.connector_id, 1, comp->drm.mode);
    if (ret) {
        fprintf(stderr, "Failed to set CRTC: %s\n", strerror(errno));
    }
    
    // Clean up
    drmModeRmFB(comp->drm.fd, fb.fb_id);
    gbm_surface_release_buffer(comp->gbm_surface, bo);
}

#include <wayland-server.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

// Protocol interfaces we need to implement
static struct wl_compositor_interface compositor_interface;
static struct wl_shell_interface shell_interface;

struct tiny_compositor {
    struct wl_display *display;
    struct wl_global *compositor_global;
    struct wl_global *shell_global;
    
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLConfig egl_config;
    
    int width;
    int height;
};

// Minimal compositor implementation
static void compositor_create_surface(struct wl_client *client,
                                    struct wl_resource *resource,
                                    uint32_t id) {
    printf("Client created surface %d\n", id);
}

static void compositor_create_region(struct wl_client *client,
                                   struct wl_resource *resource,
                                   uint32_t id) {
    printf("Client created region %d\n", id);
}

static struct wl_compositor_interface compositor_interface = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

// Minimal shell implementation
static void shell_get_shell_surface(struct wl_client *client,
                                  struct wl_resource *resource,
                                  uint32_t id,
                                  struct wl_resource *surface) {
    printf("Client requested shell surface %d\n", id);
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
    
    comp->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (comp->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return -1;
    }
    
    if (!eglInitialize(comp->egl_display, NULL, NULL)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return -1;
    }
    
    EGLint num_configs;
    if (!eglChooseConfig(comp->egl_display, config_attribs, &comp->egl_config, 1, &num_configs)) {
        fprintf(stderr, "Failed to choose EGL config\n");
        return -1;
    }
    
    comp->egl_context = eglCreateContext(comp->egl_display, comp->egl_config,
                                       EGL_NO_CONTEXT, context_attribs);
    if (comp->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        return -1;
    }
    
    return 0;
}

static void render_frame(struct tiny_compositor *comp) {
    eglMakeCurrent(comp->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, comp->egl_context);
    
    glViewport(0, 0, comp->width, comp->height);
    glClearColor(0.25f, 0.25f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    eglSwapBuffers(comp->egl_display, EGL_NO_SURFACE);
}

int main(int argc, char *argv[]) {
    struct tiny_compositor comp;
    comp.width = 1024;
    comp.height = 768;
    
    // Create Wayland display
    comp.display = wl_display_create();
    if (!comp.display) {
        fprintf(stderr, "Failed to create Wayland display\n");
        return EXIT_FAILURE;
    }
    
    // Initialize EGL
    if (init_egl(&comp) < 0) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return EXIT_FAILURE;
    }
    
    // Set up global interfaces with proper implementations
    comp.compositor_global = wl_global_create(comp.display,
                                            &wl_compositor_interface,
                                            4, &comp, bind_compositor);
    comp.shell_global = wl_global_create(comp.display,
                                       &wl_shell_interface,
                                       1, &comp, bind_shell);
    
    // Use a custom socket name
    const char *socket_name = "wayland-1";
    if (wl_display_add_socket(comp.display, socket_name)) {
        fprintf(stderr, "Failed to add socket '%s' to Wayland display\n", socket_name);
        return EXIT_FAILURE;
    }
    
    printf("Running Wayland compositor on %s\n", socket_name);
    printf("Clients can connect with: WAYLAND_DISPLAY=%s <client>\n", socket_name);
    
    // Initialize GL and print renderer info
    eglMakeCurrent(comp.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, comp.egl_context);
    printf("OpenGL ES 2.0 renderer: %s\n", glGetString(GL_RENDERER));
    
    // Main loop
    while (1) {
        wl_display_flush_clients(comp.display);
        render_frame(&comp);
        
        struct wl_event_loop *loop = wl_display_get_event_loop(comp.display);
        wl_event_loop_dispatch(loop, 16); // 16ms timeout (~60fps)
    }
    
    // Cleanup
    eglTerminate(comp.egl_display);
    wl_display_destroy(comp.display);
    
    return EXIT_SUCCESS;
}

// compositor.h - Core compositor structures and declarations
#ifndef TRIANGLES_COMPOSITOR_H
#define TRIANGLES_COMPOSITOR_H

#include <wayland-server.h>
#include <libinput.h>
#include <libudev.h>
#include <xkbcommon/xkbcommon.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stdbool.h>
#include <stdint.h>
#include <pixman-1/pixman.h>
#include <libseat.h>
#include <sys/mman.h>

// Forward declarations
struct triangles_compositor;
struct triangles_output;
struct triangles_surface;
struct triangles_view;
struct triangles_dmabuf_buffer;
struct triangles_dmabuf_params;  // defined in dmabuf.c

// ─── DMA-BUF buffer wrapper ───────────────────────────────────────────────────
// Stored as the wl_buffer user-data for every DMA-BUF-backed buffer resource.
struct triangles_dmabuf_buffer {
    struct triangles_compositor *compositor;
    EGLImageKHR                  image;
    uint32_t                     width;
    uint32_t                     height;
    uint32_t                     format;   // DRM fourcc
};

// Output structure - represents a physical display
struct triangles_output {
    struct triangles_compositor *compositor;
    struct wl_list link;
    
    // DRM/KMS
    uint32_t crtc_id;
    uint32_t connector_id;
    drmModeModeInfo mode;
    drmModeCrtc *saved_crtc;
    
    // GBM
    struct gbm_surface *gbm_surface;
    struct gbm_bo *current_bo;
    struct gbm_bo *next_bo;
    
    // EGL — persisted across frames so GBM buffer queue works correctly
    EGLSurface egl_surface;

    // Framebuffers — cached per GBM BO to avoid AddFB/RmFB every frame
    uint32_t fb_id;        // fb for current_bo
    uint32_t next_fb_id;   // fb for next_bo (pending flip)

    // Repaint scheduling
    bool needs_repaint;    // set by input/commits, cleared by repaint loop
    bool flip_pending;     // drmModePageFlip outstanding, don't queue another
    
    // Output properties
    int32_t x, y;
    int32_t width, height;
    int32_t refresh; // mHz
    
    // Fractional scaling support
    double scale;
    int32_t scale_int; // Integer part for legacy clients
    
    struct wl_global *global;
};

// Surface structure - client buffer
struct triangles_surface {
    struct triangles_compositor *compositor;
    struct wl_resource *resource;
    struct wl_list link;
    
    // Buffer management
    struct wl_resource *buffer_resource;
    struct wl_resource *pending_buffer;
    struct wl_listener buffer_destroy_listener;
    
    // Buffer properties
    int32_t buffer_width;
    int32_t buffer_height;
    uint32_t buffer_format;
    
    // Surface state
    int32_t x, y;
    double scale; // Surface scale for fractional scaling
    bool has_buffer;
    
    // Damage tracking
    pixman_region32_t damage;
    pixman_region32_t opaque;
    pixman_region32_t input;
    
    // Callbacks
    struct wl_list frame_callbacks;
    void (*commit_handler)(void *data);  // Add this line
    
    // Role-specific data
    void *role_data;
    
    // OpenGL texture
    GLuint texture;
    EGLImageKHR egl_image;
    bool is_dmabuf;  // true when current buffer is a DMA-BUF
};

// Titlebar decoration (compositor-side, no client involvement)
// Must be defined before triangles_view which embeds it by value.
struct triangles_titlebar {
    int32_t x, y;
    int32_t width;
    int32_t height;
    bool    dragging;
    int32_t drag_start_ptr_x, drag_start_ptr_y;
    int32_t drag_start_win_x, drag_start_win_y;
};

// View - positioned surface on screen
struct triangles_view {
    struct triangles_compositor *compositor;
    struct triangles_surface *surface;
    struct triangles_output *output;
    struct wl_list link;
    
    // Position and size (in output coordinates)
    int32_t x, y;
    int32_t width, height;
    
    // Transformed dimensions (for fractional scaling)
    double transform_x, transform_y;
    double transform_width, transform_height;
    
    // Compositor-side titlebar decoration
    struct triangles_titlebar titlebar;

    bool mapped;
    bool has_csd;
};

// Seat - input device grouping
struct triangles_seat {
    struct triangles_compositor *compositor;
    struct wl_global *global;
    struct wl_list link;
    
    char *name;
    uint32_t capabilities;
    
    // Per-seat resource lists (one entry per client that bound wl_keyboard/wl_pointer)
    struct wl_list keyboard_resources;  // wl_resource list via wl_resource_get_link
    struct wl_list pointer_resources;

    // Keyboard
    struct {
        struct xkb_context *context;
        struct xkb_keymap *keymap;
        struct xkb_state *state;
        char *keymap_string;   // serialized keymap sent to clients
        int   keymap_fd;       // memfd holding keymap_string
        size_t keymap_size;    // strlen + 1
        
        uint32_t *keys;        // currently held keycodes (for enter event)
        size_t num_keys;
        size_t keys_cap;

        struct triangles_surface *focus;  // surface with keyboard focus
    } keyboard;
    
    // Pointer
    struct {
        double x, y;
        struct triangles_surface *focus;  // surface under cursor
        // Titlebar drag state
        struct triangles_view *dragging_view;
        double drag_start_ptr_x, drag_start_ptr_y;
        int32_t drag_start_win_x, drag_start_win_y;
        // Cursor sprite set by client via wl_pointer.set_cursor
        struct triangles_surface *cursor_surface;
        int32_t cursor_hotspot_x, cursor_hotspot_y;
    } pointer;
    
    struct wl_list resources;  // legacy — kept for seat resource tracking
};

// Main compositor structure
struct triangles_compositor {
    // Wayland core
    struct wl_display *display;
    struct wl_event_loop *event_loop;
    struct libseat *seat;
    
    // DRM/KMS
    int drm_fd;
    struct gbm_device *gbm;
    drmModeRes *resources;
    
    // EGL
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLConfig egl_config;
    
    // libinput
    struct libinput *libinput;
    struct udev *udev;
    struct libinput_event *current_event;
    
    char *seat_id;
    
    // Lists
    struct wl_list output_list;
    struct wl_list surface_list;
    struct wl_list view_list;
    struct wl_list seat_list;
    
    // Globals
    struct wl_global *compositor_global;
    struct wl_global *subcompositor_global;
    struct wl_global *shm_global;
    struct wl_global *dmabuf_global;  // zwp_linux_dmabuf_v1
    
    // Protocol implementations
    void *xdg_shell;
    void *layer_shell;
    
    // State
    bool running;
    
    // Fractional scaling configuration
    double default_scale;
    bool fractional_scale_enabled;
};

// Function declarations
struct triangles_compositor *triangles_compositor_create(void);
bool triangles_compositor_init(struct triangles_compositor *compositor);
void triangles_compositor_destroy(struct triangles_compositor *compositor);

// Output functions
struct triangles_output *triangles_output_create(struct triangles_compositor *compositor,
                                                  uint32_t connector_id);
void triangles_output_destroy(struct triangles_output *output);
void triangles_output_repaint(struct triangles_output *output);     // immediate paint
void triangles_output_schedule_repaint(struct triangles_output *output); // deferred
void triangles_output_drm_dispatch(struct triangles_compositor *compositor);
void triangles_output_set_scale(struct triangles_output *output, double scale);

// Surface functions
struct triangles_surface *triangles_surface_create(struct wl_client *client,
                                                    struct wl_resource *compositor_resource,
                                                    uint32_t id);
void triangles_surface_destroy(struct triangles_surface *surface);
void triangles_surface_attach_buffer(struct triangles_surface *surface,
                                      struct wl_resource *buffer);
void triangles_surface_commit(struct triangles_surface *surface);

// View functions
struct triangles_view *triangles_view_create(struct triangles_surface *surface);
void triangles_view_destroy(struct triangles_view *view);
void triangles_view_map(struct triangles_view *view, int32_t x, int32_t y);
void triangles_view_unmap(struct triangles_view *view);
void triangles_view_raise(struct triangles_view *view);

// Rendering functions
bool triangles_renderer_init(struct triangles_compositor *compositor);
void triangles_renderer_begin(struct triangles_output *output);
void triangles_renderer_render_view(struct triangles_view *view);
void triangles_renderer_end(struct triangles_output *output);

// Input functions
bool triangles_input_init(struct triangles_compositor *compositor);
void triangles_input_handle_event(struct triangles_compositor *compositor,
                                   struct libinput_event *event);

// Input focus helpers (called from xdg_shell.c / pointer hit-testing)
void triangles_keyboard_set_focus(struct triangles_seat *seat,
                                   struct triangles_surface *surface);
void triangles_keyboard_send_key(struct triangles_seat *seat,
                                  uint32_t keycode,
                                  enum libinput_key_state state);
void triangles_pointer_set_focus(struct triangles_seat *seat,
                                  struct triangles_surface *surface,
                                  double sx, double sy);
void triangles_pointer_clear_focus(struct triangles_seat *seat);
void triangles_pointer_send_motion(struct triangles_seat *seat, double sx, double sy);
void triangles_pointer_send_button(struct triangles_seat *seat, uint32_t button,
                                    enum libinput_button_state state);
void triangles_pointer_send_axis(struct triangles_seat *seat,
                                  enum wl_pointer_axis axis, double value);
void triangles_pointer_update_focus(struct triangles_compositor *compositor,
                                     struct triangles_seat *seat);
void triangles_seat_update_keymap(struct triangles_seat *seat);

// Titlebar / decoration rendering
void triangles_renderer_render_titlebar(struct triangles_view *view);
void triangles_renderer_render_cursor(struct triangles_seat *seat,
                                       struct triangles_output *output);

#define TITLEBAR_HEIGHT 24

// DMA-BUF functions
bool   triangles_dmabuf_init(struct triangles_compositor *compositor);
GLuint triangles_dmabuf_import_texture(struct triangles_compositor *compositor,
                                        struct wl_resource *buffer);
void   triangles_dmabuf_buffer_destroy(struct wl_resource *resource);
GLuint triangles_dmabuf_create_texture(struct triangles_compositor *compositor,
                                        EGLImageKHR image);
EGLImageKHR triangles_dmabuf_import_eglimage(struct triangles_compositor *compositor,
                                              struct triangles_dmabuf_params *params);

#endif // TRIANGLES_COMPOSITOR_H

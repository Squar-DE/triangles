// xdg_shell.c - XDG shell protocol implementation
#include "compositor.h"
#include "xdg-shell-server-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct triangles_xdg_surface {
    struct triangles_surface *surface;
    struct wl_resource *resource;
    struct wl_listener surface_destroy_listener;
    
    void *role_object;
    
    bool configured;
    uint32_t configure_serial;
};

struct triangles_xdg_toplevel {
    struct triangles_xdg_surface *xdg_surface;
    struct wl_resource *resource;
    struct triangles_view *view;
    
    char *title;
    char *app_id;
    
    int32_t width, height;
    int32_t geom_x, geom_y;          // window geometry offset (from set_window_geometry)
    int32_t geom_width, geom_height;  // 0 = unset, use full buffer size
    bool maximized;
    bool fullscreen;
    bool activated;
};

struct triangles_xdg_popup {
    struct triangles_xdg_surface *xdg_surface;
    struct wl_resource *resource;
    
    struct triangles_xdg_surface *parent;
    int32_t x, y;
};

// xdg_wm_base interface
static void xdg_wm_base_destroy(struct wl_client *client,
                               struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void xdg_surface_handle_surface_destroy(struct wl_listener *listener,
                                              void *data) {
    struct triangles_xdg_surface *xdg_surface =
        wl_container_of(listener, xdg_surface, surface_destroy_listener);
    
    xdg_surface->surface = NULL;
}

static void xdg_wm_base_create_positioner(struct wl_client *client,
                                         struct wl_resource *resource,
                                         uint32_t id) {
    struct wl_resource *positioner_resource = wl_resource_create(client,
        &xdg_positioner_interface, wl_resource_get_version(resource), id);
    
    if (!positioner_resource) {
        wl_client_post_no_memory(client);
        return;
    }
    
    // TODO: Implement positioner
    wl_resource_set_implementation(positioner_resource, NULL, NULL, NULL);
}

static void xdg_wm_base_get_xdg_surface(struct wl_client *client,
                                       struct wl_resource *resource,
                                       uint32_t id,
                                       struct wl_resource *surface_resource) {
    struct triangles_surface *surface = wl_resource_get_user_data(surface_resource);
    
    struct triangles_xdg_surface *xdg_surface = calloc(1, sizeof(*xdg_surface));
    if (!xdg_surface) {
        wl_client_post_no_memory(client);
        return;
    }
    
    xdg_surface->surface = surface;
    xdg_surface->resource = wl_resource_create(client, &xdg_surface_interface,
                                               wl_resource_get_version(resource), id);
    if (!xdg_surface->resource) {
        free(xdg_surface);
        wl_client_post_no_memory(client);
        return;
    }
    
    xdg_surface->surface_destroy_listener.notify = xdg_surface_handle_surface_destroy;
    
    // Forward declare interface
    extern const struct xdg_surface_interface xdg_surface_implementation;
    wl_resource_set_implementation(xdg_surface->resource,
                                   &xdg_surface_implementation,
                                   xdg_surface, NULL);
    
    surface->role_data = xdg_surface;
}

static void xdg_wm_base_pong(struct wl_client *client,
                            struct wl_resource *resource,
                            uint32_t serial) {
    // Client responded to ping
}

static const struct xdg_wm_base_interface xdg_wm_base_implementation = {
    .destroy = xdg_wm_base_destroy,
    .create_positioner = xdg_wm_base_create_positioner,
    .get_xdg_surface = xdg_wm_base_get_xdg_surface,
    .pong = xdg_wm_base_pong,
};

// xdg_surface interface
static void xdg_surface_destroy(struct wl_client *client,
                               struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void xdg_toplevel_handle_surface_commit(void *data) {
    struct triangles_xdg_toplevel *toplevel = data;
    
    if (!toplevel->xdg_surface->configured) {
        // Send initial configure
        struct wl_array states;
        wl_array_init(&states);
        
        xdg_toplevel_send_configure(toplevel->resource, 0, 0, &states);
        wl_array_release(&states);
        
        xdg_surface_send_configure(toplevel->xdg_surface->resource,
                                  ++toplevel->xdg_surface->configure_serial);
        return;
    }
    
    // Map the view if it has a buffer
    if (toplevel->view && toplevel->xdg_surface->surface->has_buffer && !toplevel->view->mapped) {
        static int window_offset = 0;
        int32_t wx = 100 + window_offset;
        int32_t decoration_height = (toplevel->geom_y > 0) ? 0 : TITLEBAR_HEIGHT;
        int32_t wy = decoration_height + 100 + window_offset;
        toplevel->view->has_csd = (toplevel->geom_y > 0);
        triangles_view_map(toplevel->view, wx, wy);
        window_offset += 50;
        if (window_offset > 200) window_offset = 0;

        // Give keyboard focus on map
        struct triangles_compositor *comp = toplevel->xdg_surface->surface->compositor;
        if (!wl_list_empty(&comp->seat_list)) {
            struct triangles_seat *seat = wl_container_of(
                comp->seat_list.next, seat, link);
            triangles_keyboard_set_focus(seat, toplevel->xdg_surface->surface);
        }
        printf("Mapped toplevel view at %d, %d\n", wx, wy);
    }
}

static void xdg_surface_get_toplevel(struct wl_client *client,
                                    struct wl_resource *resource,
                                    uint32_t id) {
    struct triangles_xdg_surface *xdg_surface = wl_resource_get_user_data(resource);
    
    struct triangles_xdg_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    if (!toplevel) {
        wl_client_post_no_memory(client);
        return;
    }
    
    toplevel->xdg_surface = xdg_surface;
    xdg_surface->role_object = toplevel;
    
    toplevel->resource = wl_resource_create(client, &xdg_toplevel_interface,
                                           wl_resource_get_version(resource), id);
    if (!toplevel->resource) {
        free(toplevel);
        wl_client_post_no_memory(client);
        return;
    }
    
    // Create view for this toplevel
    toplevel->view = triangles_view_create(xdg_surface->surface);
    
    // Set commit handler
    xdg_surface->surface->commit_handler = xdg_toplevel_handle_surface_commit;
    xdg_surface->surface->role_data = toplevel;
    
    // Forward declare interface
    extern const struct xdg_toplevel_interface xdg_toplevel_implementation;
    wl_resource_set_implementation(toplevel->resource,
                                   &xdg_toplevel_implementation,
                                   toplevel, NULL);
    
    // Send initial configure
    struct wl_array states;
    wl_array_init(&states);
    xdg_toplevel_send_configure(toplevel->resource, 0, 0, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(xdg_surface->resource, ++xdg_surface->configure_serial);
}

static void xdg_surface_get_popup(struct wl_client *client,
                                 struct wl_resource *resource,
                                 uint32_t id,
                                 struct wl_resource *parent_resource,
                                 struct wl_resource *positioner_resource) {
    struct triangles_xdg_surface *xdg_surface = wl_resource_get_user_data(resource);
    
    struct triangles_xdg_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        wl_client_post_no_memory(client);
        return;
    }
    
    popup->xdg_surface = xdg_surface;
    popup->resource = wl_resource_create(client, &xdg_popup_interface,
                                        wl_resource_get_version(resource), id);
    if (!popup->resource) {
        free(popup);
        wl_client_post_no_memory(client);
        return;
    }
    
    if (parent_resource) {
        popup->parent = wl_resource_get_user_data(parent_resource);
    }
    
    // Forward declare interface
    extern const struct xdg_popup_interface xdg_popup_implementation;
    wl_resource_set_implementation(popup->resource,
                                   &xdg_popup_implementation,
                                   popup, NULL);
}

static void xdg_surface_set_window_geometry(struct wl_client *client,
                                             struct wl_resource *resource,
                                             int32_t x, int32_t y,
                                             int32_t width, int32_t height) {
    struct triangles_xdg_surface *xdg_surface = wl_resource_get_user_data(resource);
    struct triangles_xdg_toplevel *toplevel = xdg_surface->role_object;
    if (!toplevel) return;
    toplevel->geom_x      = x;
    toplevel->geom_y      = y;
    toplevel->geom_width  = width;
    toplevel->geom_height = height;
    // The geometry offset tells us the client already has its own decoration
    // at the top of the buffer — suppress our compositor titlebar if geom_y > 0
}

static void xdg_surface_ack_configure(struct wl_client *client,
                                     struct wl_resource *resource,
                                     uint32_t serial) {
    struct triangles_xdg_surface *xdg_surface = wl_resource_get_user_data(resource);
    
    if (serial == xdg_surface->configure_serial) {
        xdg_surface->configured = true;
    }
}


const struct xdg_surface_interface xdg_surface_implementation = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

// xdg_toplevel interface
static void xdg_toplevel_destroy(struct wl_client *client,
                                struct wl_resource *resource) {
    struct triangles_xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
    
    if (toplevel->view) {
        triangles_view_destroy(toplevel->view);
    }
    
    free(toplevel->title);
    free(toplevel->app_id);
    free(toplevel);
    
    wl_resource_destroy(resource);
}

static void xdg_toplevel_handle_commit(struct triangles_xdg_toplevel *toplevel) {
    if (!toplevel->xdg_surface->configured) {
        // Send initial configure
        struct wl_array states;
        wl_array_init(&states);
        
        xdg_toplevel_send_configure(toplevel->resource, 0, 0, &states);
        wl_array_release(&states);
        
        xdg_surface_send_configure(toplevel->xdg_surface->resource,
                                  ++toplevel->xdg_surface->configure_serial);
        return;
    }
    
    // Map the view if it has a buffer
    if (toplevel->view && toplevel->xdg_surface->surface->has_buffer && !toplevel->view->mapped) {
        static int window_offset = 0;
        int32_t wx = 100 + window_offset;
        int32_t wy = TITLEBAR_HEIGHT + 100 + window_offset;
        triangles_view_map(toplevel->view, wx, wy);
        window_offset += 50;
        if (window_offset > 200) window_offset = 0;

        struct triangles_compositor *comp = toplevel->xdg_surface->surface->compositor;
        if (!wl_list_empty(&comp->seat_list)) {
            struct triangles_seat *seat = wl_container_of(
                comp->seat_list.next, seat, link);
            triangles_keyboard_set_focus(seat, toplevel->xdg_surface->surface);
        }

        printf("Mapped toplevel view at %d, %d\n", wx, wy);
    }
}

static void xdg_toplevel_set_parent(struct wl_client *client,
                                   struct wl_resource *resource,
                                   struct wl_resource *parent) {
    // TODO: Handle parent relationship
}

static void xdg_toplevel_set_title(struct wl_client *client,
                                  struct wl_resource *resource,
                                  const char *title) {
    struct triangles_xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
    
    free(toplevel->title);
    toplevel->title = strdup(title);
    
    printf("Window title: %s\n", title);
}

static void xdg_toplevel_set_app_id(struct wl_client *client,
                                   struct wl_resource *resource,
                                   const char *app_id) {
    struct triangles_xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
    
    free(toplevel->app_id);
    toplevel->app_id = strdup(app_id);
    
    printf("App ID: %s\n", app_id);
}

static void xdg_toplevel_show_window_menu(struct wl_client *client,
                                         struct wl_resource *resource,
                                         struct wl_resource *seat,
                                         uint32_t serial,
                                         int32_t x, int32_t y) {
    // TODO: Show window menu
}

static void xdg_toplevel_move(struct wl_client *client,
                             struct wl_resource *resource,
                             struct wl_resource *seat,
                             uint32_t serial) {
    // TODO: Handle interactive move
}

static void xdg_toplevel_resize(struct wl_client *client,
                               struct wl_resource *resource,
                               struct wl_resource *seat,
                               uint32_t serial,
                               uint32_t edges) {
    // TODO: Handle interactive resize
}

static void xdg_toplevel_set_max_size(struct wl_client *client,
                                     struct wl_resource *resource,
                                     int32_t width, int32_t height) {
    // TODO: Store max size
}

static void xdg_toplevel_set_min_size(struct wl_client *client,
                                     struct wl_resource *resource,
                                     int32_t width, int32_t height) {
    // TODO: Store min size
}

static void xdg_toplevel_set_maximized(struct wl_client *client,
                                      struct wl_resource *resource) {
    struct triangles_xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
    toplevel->maximized = true;
    
    // Send configure event
    struct wl_array states;
    wl_array_init(&states);
    uint32_t *state = wl_array_add(&states, sizeof(uint32_t));
    *state = XDG_TOPLEVEL_STATE_MAXIMIZED;
    
    if (toplevel->view && toplevel->view->output) {
        xdg_toplevel_send_configure(resource,
                                   toplevel->view->output->width,
                                   toplevel->view->output->height,
                                   &states);
    }
    
    wl_array_release(&states);
    
    xdg_surface_send_configure(toplevel->xdg_surface->resource,
                              ++toplevel->xdg_surface->configure_serial);
}

static void xdg_toplevel_unset_maximized(struct wl_client *client,
                                        struct wl_resource *resource) {
    struct triangles_xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
    toplevel->maximized = false;
}

static void xdg_toplevel_set_fullscreen(struct wl_client *client,
                                       struct wl_resource *resource,
                                       struct wl_resource *output) {
    struct triangles_xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
    toplevel->fullscreen = true;
}

static void xdg_toplevel_unset_fullscreen(struct wl_client *client,
                                         struct wl_resource *resource) {
    struct triangles_xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
    toplevel->fullscreen = false;
}

static void xdg_toplevel_set_minimized(struct wl_client *client,
                                      struct wl_resource *resource) {
    // TODO: Handle minimize
}

const struct xdg_toplevel_interface xdg_toplevel_implementation = {
    .destroy = xdg_toplevel_destroy,
    .set_parent = xdg_toplevel_set_parent,
    .set_title = xdg_toplevel_set_title,
    .set_app_id = xdg_toplevel_set_app_id,
    .show_window_menu = xdg_toplevel_show_window_menu,
    .move = xdg_toplevel_move,
    .resize = xdg_toplevel_resize,
    .set_max_size = xdg_toplevel_set_max_size,
    .set_min_size = xdg_toplevel_set_min_size,
    .set_maximized = xdg_toplevel_set_maximized,
    .unset_maximized = xdg_toplevel_unset_maximized,
    .set_fullscreen = xdg_toplevel_set_fullscreen,
    .unset_fullscreen = xdg_toplevel_unset_fullscreen,
    .set_minimized = xdg_toplevel_set_minimized,
};

// xdg_popup interface
static void xdg_popup_destroy(struct wl_client *client,
                             struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void xdg_popup_grab(struct wl_client *client,
                          struct wl_resource *resource,
                          struct wl_resource *seat,
                          uint32_t serial) {
    // TODO: Handle popup grab
}

static void xdg_popup_reposition(struct wl_client *client,
                                struct wl_resource *resource,
                                struct wl_resource *positioner,
                                uint32_t token) {
    // TODO: Handle repositioning
}

const struct xdg_popup_interface xdg_popup_implementation = {
    .destroy = xdg_popup_destroy,
    .grab = xdg_popup_grab,
    .reposition = xdg_popup_reposition,
};

static void xdg_wm_base_bind(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &xdg_wm_base_interface, version, id);
    
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    
    wl_resource_set_implementation(resource, &xdg_wm_base_implementation,
                                   data, NULL);
}

bool triangles_xdg_shell_init(struct triangles_compositor *compositor) {
    struct wl_global *global = wl_global_create(compositor->display,
        &xdg_wm_base_interface, 3, compositor, xdg_wm_base_bind);
    
    if (!global) {
        return false;
    }
    
    compositor->xdg_shell = global;
    printf("XDG shell initialized\n");
    return true;
}

// protocol.c - Core Wayland protocol implementation
#include "compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

// ─── Helpers ──────────────────────────────────────────────────────────────────

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static struct triangles_seat *seat_from_resource(struct wl_resource *r) {
    return wl_resource_get_user_data(r);
}

// ─── wl_compositor ────────────────────────────────────────────────────────────

static void compositor_create_surface(struct wl_client *client,
                                      struct wl_resource *resource,
                                      uint32_t id) {
    triangles_surface_create(client, resource, id);
}

static void region_destroy(struct wl_client *client,
                            struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void region_add(struct wl_client *client, struct wl_resource *resource,
                        int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
    // TODO: track damage/input/opaque regions properly
}

static void region_subtract(struct wl_client *client, struct wl_resource *resource,
                              int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static const struct wl_region_interface region_interface = {
    .destroy  = region_destroy,
    .add      = region_add,
    .subtract = region_subtract,
};

static void compositor_create_region(struct wl_client *client,
                                     struct wl_resource *resource,
                                     uint32_t id) {
    struct wl_resource *region_resource = wl_resource_create(client,
        &wl_region_interface, wl_resource_get_version(resource), id);
    if (!region_resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(region_resource, &region_interface, NULL, NULL);
}

static const struct wl_compositor_interface compositor_interface = {
    .create_surface = compositor_create_surface,
    .create_region  = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id) {
    struct triangles_compositor *compositor = data;
    struct wl_resource *resource = wl_resource_create(client,
        &wl_compositor_interface, version, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &compositor_interface, compositor, NULL);
}

// ─── wl_subcompositor ─────────────────────────────────────────────────────────

static void subcompositor_destroy(struct wl_client *client,
                                  struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void subcompositor_get_subsurface(struct wl_client *client,
                                         struct wl_resource *resource,
                                         uint32_t id,
                                         struct wl_resource *surface,
                                         struct wl_resource *parent) {
    (void)resource; (void)surface; (void)parent;
    struct wl_resource *sub = wl_resource_create(client,
        &wl_subsurface_interface, 1, id);
    if (!sub) wl_client_post_no_memory(client);
    // TODO: subsurface support
}

static const struct wl_subcompositor_interface subcompositor_interface = {
    .destroy        = subcompositor_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void subcompositor_bind(struct wl_client *client, void *data,
                               uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *resource = wl_resource_create(client,
        &wl_subcompositor_interface, version, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &subcompositor_interface, NULL, NULL);
}

// ─── wl_keyboard ─────────────────────────────────────────────────────────────

static void keyboard_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface keyboard_interface = {
    .release = keyboard_release,
};

static void keyboard_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void keyboard_send_keymap(struct wl_resource *resource,
                                  struct triangles_seat *seat) {
    if (seat->keyboard.keymap_fd < 0 || seat->keyboard.keymap_size == 0) return;
    wl_keyboard_send_keymap(resource,
                            WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                            seat->keyboard.keymap_fd,
                            (uint32_t)seat->keyboard.keymap_size);
}

static void seat_get_keyboard(struct wl_client *client,
                               struct wl_resource *seat_resource,
                               uint32_t id) {
    struct triangles_seat *seat = seat_from_resource(seat_resource);

    struct wl_resource *resource = wl_resource_create(client,
        &wl_keyboard_interface, wl_resource_get_version(seat_resource), id);
    if (!resource) { wl_client_post_no_memory(client); return; }

    wl_resource_set_implementation(resource, &keyboard_interface,
                                   seat, keyboard_resource_destroy);
    wl_list_insert(&seat->keyboard_resources, wl_resource_get_link(resource));

    // Send keymap immediately on bind
    keyboard_send_keymap(resource, seat);

    // If this client already has focus, send enter right away
    if (seat->keyboard.focus) {
        struct wl_client *focus_client =
            wl_resource_get_client(seat->keyboard.focus->resource);
        if (focus_client == client) {
            struct wl_array keys_array;
            wl_array_init(&keys_array);
            if (seat->keyboard.num_keys > 0) {
                void *data = wl_array_add(&keys_array,
                    seat->keyboard.num_keys * sizeof(uint32_t));
                memcpy(data, seat->keyboard.keys,
                       seat->keyboard.num_keys * sizeof(uint32_t));
            }
            uint32_t serial = wl_display_next_serial(wl_client_get_display(client));
            wl_keyboard_send_enter(resource, serial,
                                   seat->keyboard.focus->resource, &keys_array);
            wl_array_release(&keys_array);
        }
    }
}

// ─── wl_keyboard focus API ───────────────────────────────────────────────────

void triangles_keyboard_set_focus(struct triangles_seat *seat,
                                   struct triangles_surface *surface) {
    if (!seat->keyboard.state) return;
    struct wl_display *display = seat->compositor->display;
    uint32_t serial = wl_display_next_serial(display);

    // Leave old focus
    if (seat->keyboard.focus && seat->keyboard.focus != surface) {
        struct wl_client *old_client =
            wl_resource_get_client(seat->keyboard.focus->resource);
        struct wl_resource *kbd;
        wl_resource_for_each(kbd, &seat->keyboard_resources) {
            if (wl_resource_get_client(kbd) == old_client)
                wl_keyboard_send_leave(kbd, serial,
                                       seat->keyboard.focus->resource);
        }
    }

    seat->keyboard.focus = surface;
    if (!surface) return;

    // Raise the focused window to the top of the z-order
    struct triangles_view *view;
    wl_list_for_each(view, &seat->compositor->view_list, link) {
        if (view->surface == surface) {
            triangles_view_raise(view);
            break;
        }
    }

    // Build held-keys array for enter event
    struct wl_array keys_array;
    wl_array_init(&keys_array);
    if (seat->keyboard.num_keys > 0) {
        void *data = wl_array_add(&keys_array,
            seat->keyboard.num_keys * sizeof(uint32_t));
        memcpy(data, seat->keyboard.keys,
               seat->keyboard.num_keys * sizeof(uint32_t));
    }

struct wl_client *new_client = wl_resource_get_client(surface->resource);

    // Compute modifier state once (guard against state not yet initialized)
    xkb_mod_mask_t depressed = 0, latched = 0, locked = 0;
    xkb_layout_index_t group = 0;
    if (seat->keyboard.state) {
        depressed = xkb_state_serialize_mods(seat->keyboard.state, XKB_STATE_MODS_DEPRESSED);
        latched   = xkb_state_serialize_mods(seat->keyboard.state, XKB_STATE_MODS_LATCHED);
        locked    = xkb_state_serialize_mods(seat->keyboard.state, XKB_STATE_MODS_LOCKED);
        group     = xkb_state_serialize_layout(seat->keyboard.state, XKB_STATE_LAYOUT_EFFECTIVE);
    }

    uint32_t mod_serial = wl_display_next_serial(display);
    struct wl_resource *kbd;
    wl_resource_for_each(kbd, &seat->keyboard_resources) {
        if (wl_resource_get_client(kbd) != new_client) continue;
        wl_keyboard_send_enter(kbd, serial, surface->resource, &keys_array);
        wl_keyboard_send_modifiers(kbd, mod_serial, depressed, latched, locked, group);
    }
    wl_array_release(&keys_array);
}

void triangles_keyboard_send_key(struct triangles_seat *seat,
                                  uint32_t keycode,
                                  enum libinput_key_state state) {
    if (!seat->keyboard.focus) return;

    struct wl_client *client =
        wl_resource_get_client(seat->keyboard.focus->resource);
    struct wl_display *display = seat->compositor->display;
    uint32_t serial   = wl_display_next_serial(display);
    uint32_t time     = now_ms();
    uint32_t wl_state = (state == LIBINPUT_KEY_STATE_PRESSED)
                        ? WL_KEYBOARD_KEY_STATE_PRESSED
                        : WL_KEYBOARD_KEY_STATE_RELEASED;

    // Update held-key array
    if (state == LIBINPUT_KEY_STATE_PRESSED) {
        if (seat->keyboard.num_keys >= seat->keyboard.keys_cap) {
            seat->keyboard.keys_cap =
                seat->keyboard.keys_cap ? seat->keyboard.keys_cap * 2 : 8;
            seat->keyboard.keys = realloc(seat->keyboard.keys,
                seat->keyboard.keys_cap * sizeof(uint32_t));
        }
        seat->keyboard.keys[seat->keyboard.num_keys++] = keycode;
    } else {
        for (size_t i = 0; i < seat->keyboard.num_keys; i++) {
            if (seat->keyboard.keys[i] == keycode) {
                seat->keyboard.keys[i] =
                    seat->keyboard.keys[--seat->keyboard.num_keys];
                break;
            }
        }
    }

    // Serialize XKB modifier state
    xkb_mod_mask_t depressed =
        xkb_state_serialize_mods(seat->keyboard.state, XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t latched =
        xkb_state_serialize_mods(seat->keyboard.state, XKB_STATE_MODS_LATCHED);
    xkb_mod_mask_t locked =
        xkb_state_serialize_mods(seat->keyboard.state, XKB_STATE_MODS_LOCKED);
    xkb_layout_index_t group =
        xkb_state_serialize_layout(seat->keyboard.state,
                                   XKB_STATE_LAYOUT_EFFECTIVE);

    struct wl_resource *kbd;
    wl_resource_for_each(kbd, &seat->keyboard_resources) {
        if (wl_resource_get_client(kbd) != client) continue;
        wl_keyboard_send_key(kbd, serial, time, keycode, wl_state);
        wl_keyboard_send_modifiers(kbd, serial,
                                   depressed, latched, locked, group);
    }
}

// ─── wl_pointer ───────────────────────────────────────────────────────────────

static void pointer_set_cursor(struct wl_client *client,
                                struct wl_resource *resource,
                                uint32_t serial,
                                struct wl_resource *surface_resource,
                                int32_t hotspot_x,
                                int32_t hotspot_y) {
    (void)serial;
    struct triangles_seat *seat = wl_resource_get_user_data(resource);

    // Only the currently focused client may set the cursor
    if (seat->pointer.focus) {
        struct wl_client *focus_client =
            wl_resource_get_client(seat->pointer.focus->resource);
        if (focus_client != client) return;
    }

    if (surface_resource) {
        struct triangles_surface *surface =
            wl_resource_get_user_data(surface_resource);
        seat->pointer.cursor_surface   = surface;
        seat->pointer.cursor_hotspot_x = hotspot_x;
        seat->pointer.cursor_hotspot_y = hotspot_y;
    } else {
        // NULL surface = hide cursor
        seat->pointer.cursor_surface = NULL;
    }
// Schedule repaint so new cursor appears immediately
    struct triangles_compositor *comp = seat->compositor;
    if (!wl_list_empty(&comp->output_list)) {
        struct triangles_output *output = wl_container_of(
            comp->output_list.next, output, link);
        triangles_output_schedule_repaint(output);
    }
}

static void pointer_release(struct wl_client *client,
                              struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_pointer_interface pointer_interface = {
    .set_cursor = pointer_set_cursor,
    .release    = pointer_release,
};

static void pointer_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void seat_get_pointer(struct wl_client *client,
                              struct wl_resource *seat_resource,
                              uint32_t id) {
    struct triangles_seat *seat = seat_from_resource(seat_resource);

    struct wl_resource *resource = wl_resource_create(client,
        &wl_pointer_interface, wl_resource_get_version(seat_resource), id);
    if (!resource) { wl_client_post_no_memory(client); return; }

    wl_resource_set_implementation(resource, &pointer_interface,
                                   seat, pointer_resource_destroy);
    wl_list_insert(&seat->pointer_resources, wl_resource_get_link(resource));
}

// ─── wl_pointer focus / event API ────────────────────────────────────────────

// Returns the topmost mapped view under (px,py), and surface-local coords
static struct triangles_view *view_at(struct triangles_compositor *compositor,
                                       double px, double py,
                                       double *sx, double *sy) {
    struct triangles_view *view;
    wl_list_for_each(view, &compositor->view_list, link) {
        if (!view->mapped || !view->surface || !view->surface->has_buffer)
            continue;
        // Titlebar hit — keep pointer focus on this surface but suppress
        // surface-local coord so client doesn't see clicks inside decoration
        int32_t ty = view->y - TITLEBAR_HEIGHT;
        if (px >= view->x && px < view->x + view->width &&
            py >= ty       && py < ty + TITLEBAR_HEIGHT) {
            *sx = px - view->x;
            *sy = 0;  // clamp to top of surface, not negative
            return view;
        }
        // Surface body
        if (px >= view->x && px < view->x + view->width &&
            py >= view->y && py < view->y + view->height) {
            *sx = px - view->x;
            *sy = py - view->y;
            return view;
        }
    }
    return NULL;
}

void triangles_pointer_set_focus(struct triangles_seat *seat,
                                  struct triangles_surface *surface,
                                  double sx, double sy) {
    struct wl_display *display = seat->compositor->display;

    if (seat->pointer.focus == surface) {
        if (!surface) return;
        // Same focus — just send motion
        struct wl_client *client = wl_resource_get_client(surface->resource);
        uint32_t time = now_ms();
        struct wl_resource *ptr;
        wl_resource_for_each(ptr, &seat->pointer_resources) {
            if (wl_resource_get_client(ptr) != client) continue;
            wl_pointer_send_motion(ptr, time,
                                   wl_fixed_from_double(sx),
                                   wl_fixed_from_double(sy));
            wl_pointer_send_frame(ptr);
        }
        return;
    }

    uint32_t serial = wl_display_next_serial(display);

    // Leave old
    if (seat->pointer.focus) {
        struct wl_client *old = wl_resource_get_client(seat->pointer.focus->resource);
        struct wl_resource *ptr;
        wl_resource_for_each(ptr, &seat->pointer_resources) {
            if (wl_resource_get_client(ptr) != old) continue;
            wl_pointer_send_leave(ptr, serial, seat->pointer.focus->resource);
            wl_pointer_send_frame(ptr);
        }
    }

    seat->pointer.focus = surface;
    if (!surface) return;

    struct wl_client *new_client = wl_resource_get_client(surface->resource);
    struct wl_resource *ptr;
    wl_resource_for_each(ptr, &seat->pointer_resources) {
        if (wl_resource_get_client(ptr) != new_client) continue;
        wl_pointer_send_enter(ptr, serial, surface->resource,
                              wl_fixed_from_double(sx),
                              wl_fixed_from_double(sy));
        wl_pointer_send_frame(ptr);
    }
}

void triangles_pointer_clear_focus(struct triangles_seat *seat) {
    triangles_pointer_set_focus(seat, NULL, 0, 0);
}

void triangles_pointer_send_motion(struct triangles_seat *seat, double sx, double sy) {
    if (!seat->pointer.focus) return;
    struct wl_client *client = wl_resource_get_client(seat->pointer.focus->resource);
    uint32_t time = now_ms();
    struct wl_resource *ptr;
    wl_resource_for_each(ptr, &seat->pointer_resources) {
        if (wl_resource_get_client(ptr) != client) continue;
        wl_pointer_send_motion(ptr, time,
                               wl_fixed_from_double(sx),
                               wl_fixed_from_double(sy));
        wl_pointer_send_frame(ptr);
    }
}

void triangles_pointer_send_button(struct triangles_seat *seat, uint32_t button,
                                    enum libinput_button_state state) {
    if (!seat->pointer.focus) return;
    struct wl_client *client = wl_resource_get_client(seat->pointer.focus->resource);
    uint32_t serial   = wl_display_next_serial(seat->compositor->display);
    uint32_t time     = now_ms();
    uint32_t wl_state = (state == LIBINPUT_BUTTON_STATE_PRESSED)
                        ? WL_POINTER_BUTTON_STATE_PRESSED
                        : WL_POINTER_BUTTON_STATE_RELEASED;
    struct wl_resource *ptr;
    wl_resource_for_each(ptr, &seat->pointer_resources) {
        if (wl_resource_get_client(ptr) != client) continue;
        wl_pointer_send_button(ptr, serial, time, button, wl_state);
        wl_pointer_send_frame(ptr);
    }
}

void triangles_pointer_send_axis(struct triangles_seat *seat,
                                  enum wl_pointer_axis axis, double value) {
    if (!seat->pointer.focus) return;
    struct wl_client *client = wl_resource_get_client(seat->pointer.focus->resource);
    uint32_t time = now_ms();
    struct wl_resource *ptr;
    wl_resource_for_each(ptr, &seat->pointer_resources) {
        if (wl_resource_get_client(ptr) != client) continue;
        wl_pointer_send_axis(ptr, time, axis, wl_fixed_from_double(value));
        wl_pointer_send_frame(ptr);
    }
}

void triangles_pointer_update_focus(struct triangles_compositor *compositor,
                                     struct triangles_seat *seat) {
    double sx, sy;
    struct triangles_view *view = view_at(compositor,
                                          seat->pointer.x, seat->pointer.y,
                                          &sx, &sy);
    triangles_pointer_set_focus(seat, view ? view->surface : NULL, sx, sy);
}

// ─── wl_seat ──────────────────────────────────────────────────────────────────

static void seat_get_touch(struct wl_client *client, struct wl_resource *resource,
                            uint32_t id) {
    struct wl_resource *touch = wl_resource_create(client,
        &wl_touch_interface, wl_resource_get_version(resource), id);
    if (!touch) wl_client_post_no_memory(client);
    wl_resource_set_implementation(touch, NULL, NULL, NULL);
}

static void seat_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_seat_interface seat_interface = {
    .get_pointer  = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch    = seat_get_touch,
    .release      = seat_release,
};

static void seat_bind(struct wl_client *client, void *data,
                      uint32_t version, uint32_t id) {
    struct triangles_seat *seat = data;
    struct wl_resource *resource = wl_resource_create(client,
        &wl_seat_interface, version, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &seat_interface, seat, NULL);
    wl_seat_send_capabilities(resource, seat->capabilities);
    if (version >= WL_SEAT_NAME_SINCE_VERSION)
        wl_seat_send_name(resource, seat->name);
}

// ─── Keymap memfd builder ─────────────────────────────────────────────────────

static bool seat_build_keymap(struct triangles_seat *seat) {
    if (!seat->keyboard.keymap) return false;

    char *str = xkb_keymap_get_as_string(seat->keyboard.keymap,
                                          XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!str) return false;

    size_t size = strlen(str) + 1;

    int fd = memfd_create("xkb-keymap", MFD_CLOEXEC);
    if (fd < 0) { free(str); return false; }

    if (ftruncate(fd, (off_t)size) < 0) { close(fd); free(str); return false; }

    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) { close(fd); free(str); return false; }
    memcpy(ptr, str, size);
    munmap(ptr, size);
    free(str);

    if (seat->keyboard.keymap_fd >= 0) close(seat->keyboard.keymap_fd);
    seat->keyboard.keymap_fd   = fd;
    seat->keyboard.keymap_size = size;
    return true;
}

void triangles_seat_update_keymap(struct triangles_seat *seat) {
    if (!seat_build_keymap(seat)) {
        fprintf(stderr, "[SEAT] Failed to build keymap memfd\n");
        return;
    }
    struct wl_resource *kbd;
    wl_resource_for_each(kbd, &seat->keyboard_resources)
        keyboard_send_keymap(kbd, seat);
    printf("[SEAT] Keymap built and distributed (%zu bytes)\n",
           seat->keyboard.keymap_size);
}

// ─── wl_data_device_manager ───────────────────────────────────────────────────
// Clients bind this to get clipboard/DnD support. We stub it so they don't
// crash on bind, but don't implement actual data transfer yet.

static void data_device_release(struct wl_client *client,
                                 struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void data_device_start_drag(struct wl_client *c, struct wl_resource *r,
                                    struct wl_resource *src, struct wl_resource *origin,
                                    struct wl_resource *icon, uint32_t serial) {
    (void)c;(void)r;(void)src;(void)origin;(void)icon;(void)serial;
}
static void data_device_set_selection(struct wl_client *c, struct wl_resource *r,
                                       struct wl_resource *src, uint32_t serial) {
    (void)c;(void)r;(void)src;(void)serial;
}

static const struct wl_data_device_interface data_device_interface = {
    .start_drag    = data_device_start_drag,
    .set_selection = data_device_set_selection,
    .release       = data_device_release,
};

static void data_source_offer(struct wl_client *c, struct wl_resource *r,
                               const char *mime) { (void)c;(void)r;(void)mime; }
static void data_source_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void data_source_set_actions(struct wl_client *c, struct wl_resource *r,
                                     uint32_t a) { (void)c;(void)r;(void)a; }

static const struct wl_data_source_interface data_source_interface = {
    .offer       = data_source_offer,
    .destroy     = data_source_destroy,
    .set_actions = data_source_set_actions,
};

static void ddm_create_data_source(struct wl_client *client,
                                    struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct wl_resource *src = wl_resource_create(client,
        &wl_data_source_interface, 3, id);
    if (!src) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(src, &data_source_interface, NULL, NULL);
}

static void ddm_get_data_device(struct wl_client *client,
                                 struct wl_resource *resource,
                                 uint32_t id,
                                 struct wl_resource *seat_resource) {
    (void)resource; (void)seat_resource;
    struct wl_resource *dev = wl_resource_create(client,
        &wl_data_device_interface, 3, id);
    if (!dev) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(dev, &data_device_interface, NULL, NULL);
}

static const struct wl_data_device_manager_interface ddm_interface = {
    .create_data_source = ddm_create_data_source,
    .get_data_device    = ddm_get_data_device,
};

static void ddm_bind(struct wl_client *client, void *data,
                     uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *resource = wl_resource_create(client,
        &wl_data_device_manager_interface, version, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &ddm_interface, NULL, NULL);
}

// ─── Protocol init ────────────────────────────────────────────────────────────

bool triangles_compositor_protocol_init(struct triangles_compositor *compositor) {
    compositor->compositor_global = wl_global_create(compositor->display,
        &wl_compositor_interface, 4, compositor, compositor_bind);
    if (!compositor->compositor_global) return false;

    compositor->subcompositor_global = wl_global_create(compositor->display,
        &wl_subcompositor_interface, 1, compositor, subcompositor_bind);
    if (!compositor->subcompositor_global) return false;

    printf("[PROTOCOL] Initializing wl_shm...\n"); fflush(stdout);
    if (wl_display_init_shm(compositor->display) < 0) {
        fprintf(stderr, "[PROTOCOL] Failed to init SHM\n"); return false;
    }
    uint32_t formats[] = { WL_SHM_FORMAT_ARGB8888, WL_SHM_FORMAT_XRGB8888 };
    for (size_t i = 0; i < sizeof(formats)/sizeof(formats[0]); i++)
        wl_display_add_shm_format(compositor->display, formats[i]);
    printf("[PROTOCOL] wl_shm initialized\n"); fflush(stdout);

    struct triangles_seat *seat = calloc(1, sizeof(*seat));
    if (!seat) return false;

    seat->compositor        = compositor;
    seat->name              = strdup("seat0");
    seat->capabilities      = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
    seat->keyboard.keymap_fd = -1;

    wl_list_init(&seat->resources);
    wl_list_init(&seat->keyboard_resources);
    wl_list_init(&seat->pointer_resources);

    seat->global = wl_global_create(compositor->display,
        &wl_seat_interface, 7, seat, seat_bind);
    if (!seat->global) { free(seat->name); free(seat); return false; }

    wl_list_insert(&compositor->seat_list, &seat->link);

    // wl_data_device_manager — needed by most clients even if clipboard is stubbed
    if (!wl_global_create(compositor->display,
            &wl_data_device_manager_interface, 3, NULL, ddm_bind)) {
        fprintf(stderr, "[PROTOCOL] Failed to create wl_data_device_manager\n");
        return false;
    }

    printf("Core Wayland protocols initialized\n");
    return true;
}

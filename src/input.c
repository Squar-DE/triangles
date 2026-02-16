// input.c - Input handling with libinput and libseat
#include "compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

static void handle_enable_seat(struct libseat *seat, void *data) {
    (void)seat;
    (void)data;
    printf("Seat enabled\n");
}

static void handle_disable_seat(struct libseat *seat, void *data) {
    struct triangles_compositor *compositor = data;
    libseat_disable_seat(compositor->seat);
    printf("Seat disabled\n");
}

static const struct libseat_seat_listener seat_listener = {
    .enable_seat = handle_enable_seat,
    .disable_seat = handle_disable_seat,
};

static int open_restricted(const char *path, int flags, void *user_data) {
    struct triangles_compositor *compositor = user_data;
    int fd;
    
    int device_id = libseat_open_device(compositor->seat, path, &fd);
    if (device_id < 0) {
        fprintf(stderr, "Failed to open %s via libseat: %d\n", path, device_id);
        return -1;
    }
    
    // Store device_id somewhere if you need to close it later
    // For now, just return the fd
    return fd;
}

static void close_restricted(int fd, void *user_data) {
    close(fd);
}

static const struct libinput_interface libinput_interface_impl = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

static int libseat_source_dispatch(int fd, uint32_t mask, void *data) {
    struct triangles_compositor *compositor = data;
    (void)fd;
    (void)mask;
    
    if (libseat_dispatch(compositor->seat, 0) < 0) {
        fprintf(stderr, "libseat dispatch failed\n");
    }
    
    return 0;
}

static int libinput_source_dispatch(int fd, uint32_t mask, void *data) {
    struct triangles_compositor *compositor = data;
    (void)fd;
    (void)mask;
    
    if (libinput_dispatch(compositor->libinput) != 0) {
        fprintf(stderr, "libinput dispatch failed\n");
        return 0;
    }
    
    struct libinput_event *event;
    while ((event = libinput_get_event(compositor->libinput))) {
        triangles_input_handle_event(compositor);
        libinput_event_destroy(event);
    }
    
    return 0;
}

bool triangles_input_init(struct triangles_compositor *compositor) {
    // Initialize libseat
    compositor->seat = libseat_open_seat(&seat_listener, compositor);
    if (!compositor->seat) {
        fprintf(stderr, "Failed to open seat\n");
        return false;
    }
    
    printf("Libseat opened\n");
    
    // Add libseat fd to event loop
    int seat_fd = libseat_get_fd(compositor->seat);
    struct wl_event_source *seat_source = wl_event_loop_add_fd(compositor->event_loop,
                                                               seat_fd, WL_EVENT_READABLE,
                                                               libseat_source_dispatch,
                                                               compositor);
    if (!seat_source) {
        fprintf(stderr, "Failed to add libseat to event loop\n");
        libseat_close_seat(compositor->seat);
        return false;
    }
    
    // Dispatch libseat to get initial seat state
    libseat_dispatch(compositor->seat, 0);
    
    // Initialize libinput with udev
    compositor->libinput = libinput_udev_create_context(&libinput_interface_impl,
                                                        compositor,
                                                        compositor->udev);
    if (!compositor->libinput) {
        fprintf(stderr, "Failed to create libinput context\n");
        libseat_close_seat(compositor->seat);
        return false;
    }
    
    // Assign seat
    const char *seat_name = libseat_seat_name(compositor->seat);
    if (!seat_name) {
        seat_name = "seat0";
    }
    
    if (libinput_udev_assign_seat(compositor->libinput, seat_name) != 0) {
        fprintf(stderr, "Failed to assign seat\n");
        libinput_unref(compositor->libinput);
        libseat_close_seat(compositor->seat);
        compositor->libinput = NULL;
        return false;
    }
    
    // Add libinput fd to event loop
    int fd = libinput_get_fd(compositor->libinput);
    struct wl_event_source *source = wl_event_loop_add_fd(compositor->event_loop,
                                                          fd, WL_EVENT_READABLE,
                                                          libinput_source_dispatch,
                                                          compositor);
    if (!source) {
        fprintf(stderr, "Failed to add libinput to event loop\n");
        libinput_unref(compositor->libinput);
        libseat_close_seat(compositor->seat);
        compositor->libinput = NULL;
        return false;
    }
    
    // Initialize keyboard state
    if (!wl_list_empty(&compositor->seat_list)) {
        struct triangles_seat *seat = wl_container_of(compositor->seat_list.next,
                                                      seat, link);
        seat->keyboard.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (seat->keyboard.context) {
            struct xkb_rule_names names = {
                .rules = NULL,
                .model = NULL,
                .layout = "us",
                .variant = NULL,
                .options = NULL,
            };
            
            seat->keyboard.keymap = xkb_keymap_new_from_names(seat->keyboard.context,
                                                              &names,
                                                              XKB_KEYMAP_COMPILE_NO_FLAGS);
            if (seat->keyboard.keymap) {
                seat->keyboard.state = xkb_state_new(seat->keyboard.keymap);
            }
        }
    }
    
    printf("Input system initialized\n");
    return true;
}

static void handle_pointer_motion(struct triangles_compositor *compositor,
                                  struct libinput_event_pointer *event) {
    if (wl_list_empty(&compositor->seat_list)) return;
    
    struct triangles_seat *seat = wl_container_of(compositor->seat_list.next,
                                                  seat, link);
    
    double dx = libinput_event_pointer_get_dx(event);
    double dy = libinput_event_pointer_get_dy(event);
    
    seat->pointer.x += dx;
    seat->pointer.y += dy;
    
    // Clamp to first output bounds
    if (!wl_list_empty(&compositor->output_list)) {
        struct triangles_output *output = wl_container_of(compositor->output_list.next,
                                                          output, link);
        if (seat->pointer.x < 0) seat->pointer.x = 0;
        if (seat->pointer.y < 0) seat->pointer.y = 0;
        if (seat->pointer.x >= output->width) seat->pointer.x = output->width - 1;
        if (seat->pointer.y >= output->height) seat->pointer.y = output->height - 1;
    }
    
    // TODO: Send pointer motion events to focused surface
    printf("Pointer: %.2f, %.2f\n", seat->pointer.x, seat->pointer.y);
}

static void handle_pointer_button(struct triangles_compositor *compositor,
                                  struct libinput_event_pointer *event) {
    (void)compositor;
    uint32_t button = libinput_event_pointer_get_button(event);
    enum libinput_button_state state = libinput_event_pointer_get_button_state(event);
    
    printf("Button %u %s\n", button, state == LIBINPUT_BUTTON_STATE_PRESSED ? "pressed" : "released");
    
    // TODO: Handle button press/release, focus surfaces, etc.
}

static void handle_pointer_axis(struct triangles_compositor *compositor,
                               struct libinput_event_pointer *event) {
    (void)compositor;
    // Handle scroll wheel
    if (libinput_event_pointer_has_axis(event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
        double value = libinput_event_pointer_get_axis_value(event,
                                                             LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
        printf("Scroll vertical: %.2f\n", value);
    }
    
    if (libinput_event_pointer_has_axis(event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)) {
        double value = libinput_event_pointer_get_axis_value(event,
                                                             LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
        printf("Scroll horizontal: %.2f\n", value);
    }
}

static void handle_keyboard_key(struct triangles_compositor *compositor,
                               struct libinput_event_keyboard *event) {
    uint32_t keycode = libinput_event_keyboard_get_key(event);
    enum libinput_key_state state = libinput_event_keyboard_get_key_state(event);
    
    if (wl_list_empty(&compositor->seat_list)) return;
    
    struct triangles_seat *seat = wl_container_of(compositor->seat_list.next,
                                                  seat, link);
    
    if (!seat->keyboard.state) return;
    
    // Update XKB state
    xkb_keycode_t xkb_keycode = keycode + 8;
    xkb_state_update_key(seat->keyboard.state, xkb_keycode,
                        state == LIBINPUT_KEY_STATE_PRESSED ? 
                        XKB_KEY_DOWN : XKB_KEY_UP);
    
    // Get keysym
    xkb_keysym_t keysym = xkb_state_key_get_one_sym(seat->keyboard.state, xkb_keycode);
    char name[64];
    xkb_keysym_get_name(keysym, name, sizeof(name));
    
    printf("Key %s: %s\n", name, state == LIBINPUT_KEY_STATE_PRESSED ? "pressed" : "released");
    
    // Handle compositor shortcuts
    if (state == LIBINPUT_KEY_STATE_PRESSED) {
        // Ctrl+Alt+Backspace to quit
        if (keysym == XKB_KEY_BackSpace &&
            xkb_state_mod_name_is_active(seat->keyboard.state, XKB_MOD_NAME_CTRL,
                                        XKB_STATE_MODS_EFFECTIVE) &&
            xkb_state_mod_name_is_active(seat->keyboard.state, XKB_MOD_NAME_ALT,
                                        XKB_STATE_MODS_EFFECTIVE)) {
            printf("Shutting down compositor (Ctrl+Alt+Backspace)\n");
            compositor->running = false;
        }
    }
    
    // TODO: Send keyboard events to focused surface
}

void triangles_input_handle_event(struct triangles_compositor *compositor) {
    struct libinput_event *event = libinput_get_event(compositor->libinput);
    if (!event) return;
    
    enum libinput_event_type type = libinput_event_get_type(event);
    
    switch (type) {
        case LIBINPUT_EVENT_DEVICE_ADDED: {
            struct libinput_device *device = libinput_event_get_device(event);
            printf("Input device added: %s\n", libinput_device_get_name(device));
            break;
        }
        
        case LIBINPUT_EVENT_DEVICE_REMOVED: {
            struct libinput_device *device = libinput_event_get_device(event);
            printf("Input device removed: %s\n", libinput_device_get_name(device));
            break;
        }
        
        case LIBINPUT_EVENT_POINTER_MOTION:
            handle_pointer_motion(compositor, libinput_event_get_pointer_event(event));
            break;
        
        case LIBINPUT_EVENT_POINTER_BUTTON:
            handle_pointer_button(compositor, libinput_event_get_pointer_event(event));
            break;
        
        case LIBINPUT_EVENT_POINTER_AXIS:
            handle_pointer_axis(compositor, libinput_event_get_pointer_event(event));
            break;
        
        case LIBINPUT_EVENT_KEYBOARD_KEY:
            handle_keyboard_key(compositor, libinput_event_get_keyboard_event(event));
            break;
        
        default:
            break;
    }
}

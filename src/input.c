// input.c - Input handling: libinput events → Wayland protocol events
#include "compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

// ─── libseat callbacks ────────────────────────────────────────────────────────

static void handle_enable_seat(struct libseat *seat, void *data) {
    (void)seat; (void)data;
    printf("Seat enabled\n");
}

static void handle_disable_seat(struct libseat *seat, void *data) {
    struct triangles_compositor *compositor = data;
    libseat_disable_seat(compositor->seat);
    printf("Seat disabled\n");
}

static const struct libseat_seat_listener seat_listener = {
    .enable_seat  = handle_enable_seat,
    .disable_seat = handle_disable_seat,
};

// ─── libinput open/close (via libseat) ───────────────────────────────────────

static int open_restricted(const char *path, int flags, void *user_data) {
    struct triangles_compositor *compositor = user_data;
    int fd;
    int device_id = libseat_open_device(compositor->seat, path, &fd);
    printf("[LIBSEAT] open_restricted: path=%s device_id=%d fd=%d\n",
           path, device_id, fd);  // ← add this
    fflush(stdout);
    if (device_id < 0) {
        fprintf(stderr, "Failed to open %s via libseat: %d\n", path, device_id);
        return -1;
    }
    return fd;
}

static void close_restricted(int fd, void *user_data) {
    (void)user_data;
    close(fd);
}

static const struct libinput_interface libinput_interface_impl = {
    .open_restricted  = open_restricted,
    .close_restricted = close_restricted,
};

// ─── Event loop dispatch callbacks ───────────────────────────────────────────

static int libseat_source_dispatch(int fd, uint32_t mask, void *data) {
    (void)fd; (void)mask;
    struct triangles_compositor *compositor = data;
    if (libseat_dispatch(compositor->seat, 0) < 0)
        fprintf(stderr, "libseat dispatch failed\n");
    return 0;
}

static int libinput_source_dispatch(int fd, uint32_t mask, void *data) {
    (void)fd; (void)mask;
    struct triangles_compositor *compositor = data;
    if (libinput_dispatch(compositor->libinput) != 0) {
        fprintf(stderr, "libinput dispatch failed\n");
        return 0;
    }
    struct libinput_event *event;
    while ((event = libinput_get_event(compositor->libinput))) {
        triangles_input_handle_event(compositor, event);
        libinput_event_destroy(event);
    }
    return 0;
}

// ─── Helper: get first seat ───────────────────────────────────────────────────

static struct triangles_seat *get_seat(struct triangles_compositor *compositor) {
    if (wl_list_empty(&compositor->seat_list)) return NULL;
    struct triangles_seat *seat;
    seat = wl_container_of(compositor->seat_list.next, seat, link);
    return seat;
}

// ─── Pointer motion ──────────────────────────────────────────────────────────

static void handle_pointer_motion_absolute(struct triangles_compositor *compositor,
                                            struct libinput_event_pointer *event) {
    struct triangles_seat *seat = get_seat(compositor);
    if (!seat) return;

    if (wl_list_empty(&compositor->output_list)) return;
    struct triangles_output *output = wl_container_of(
        compositor->output_list.next, output, link);

    // Absolute coords are in [0,1] normalised to the output dimensions
    seat->pointer.x = libinput_event_pointer_get_absolute_x_transformed(
                          event, output->width);
    seat->pointer.y = libinput_event_pointer_get_absolute_y_transformed(
                          event, output->height);

    if (seat->pointer.dragging_view) {
        struct triangles_view *view = seat->pointer.dragging_view;
        view->x = (int32_t)(seat->pointer.drag_start_win_x +
                            (seat->pointer.x - seat->pointer.drag_start_ptr_x));
        view->y = (int32_t)(seat->pointer.drag_start_win_y +
                            (seat->pointer.y - seat->pointer.drag_start_ptr_y));
        if (view->output) triangles_output_schedule_repaint(view->output);
        return;
    }

    triangles_pointer_update_focus(compositor, seat);
    triangles_output_schedule_repaint(output);
}

static void handle_pointer_motion(struct triangles_compositor *compositor,
                                   struct libinput_event_pointer *event) {
    struct triangles_seat *seat = get_seat(compositor);
    if (!seat) return;

    double dx = libinput_event_pointer_get_dx(event);
    double dy = libinput_event_pointer_get_dy(event);

    seat->pointer.x += dx;
    seat->pointer.y += dy;

    // Clamp to output bounds
    if (!wl_list_empty(&compositor->output_list)) {
        struct triangles_output *output = wl_container_of(
            compositor->output_list.next, output, link);
        if (seat->pointer.x < 0) seat->pointer.x = 0;
        if (seat->pointer.y < 0) seat->pointer.y = 0;
        if (seat->pointer.x >= output->width)  seat->pointer.x = output->width  - 1;
        if (seat->pointer.y >= output->height) seat->pointer.y = output->height - 1;
    }

    // Handle titlebar drag
    if (seat->pointer.dragging_view) {
        struct triangles_view *view = seat->pointer.dragging_view;
        view->x = (int32_t)(seat->pointer.drag_start_win_x +
                            (seat->pointer.x - seat->pointer.drag_start_ptr_x));
        view->y = (int32_t)(seat->pointer.drag_start_win_y +
                            (seat->pointer.y - seat->pointer.drag_start_ptr_y));
        if (view->output) triangles_output_schedule_repaint(view->output);
        return;
    }

    // Update pointer focus and send motion/enter/leave to client
    triangles_pointer_update_focus(compositor, seat);

    // Schedule repaint so the cursor sprite moves — deferred to avoid
    // blocking the input event queue with synchronous GPU work
    if (!wl_list_empty(&compositor->output_list)) {
        struct triangles_output *output = wl_container_of(
            compositor->output_list.next, output, link);
        triangles_output_schedule_repaint(output);
    }
}

// ─── Pointer button ──────────────────────────────────────────────────────────

static void handle_pointer_button(struct triangles_compositor *compositor,
                                   struct libinput_event_pointer *event) {
    struct triangles_seat *seat = get_seat(compositor);
    if (!seat) return;

    uint32_t button = libinput_event_pointer_get_button(event);
    enum libinput_button_state state = libinput_event_pointer_get_button_state(event);

    if (state == LIBINPUT_BUTTON_STATE_PRESSED) {
        double px = seat->pointer.x;
        double py = seat->pointer.y;

        // Single hit-test respecting z-order: walk front-to-back, check
        // titlebar strip first then surface body for each view in order.
        struct triangles_view *hit_view = NULL;
        bool hit_titlebar = false;

        struct triangles_view *view;
        wl_list_for_each(view, &compositor->view_list, link) {
            if (!view->mapped) continue;

            int32_t ty = view->y - TITLEBAR_HEIGHT;
            if (px >= view->x && px < view->x + view->width &&
                py >= ty       && py < ty + TITLEBAR_HEIGHT) {
                hit_view     = view;
                hit_titlebar = true;
                break;
            }
            if (px >= view->x && px < view->x + view->width &&
                py >= view->y  && py < view->y + view->height) {
                hit_view     = view;
                hit_titlebar = false;
                break;
            }
        }

        if (hit_view) {
            // Raise and focus regardless of titlebar vs surface
            triangles_keyboard_set_focus(seat, hit_view->surface);

            if (hit_titlebar && button == BTN_LEFT) {
                seat->pointer.dragging_view    = hit_view;
                seat->pointer.drag_start_ptr_x = px;
                seat->pointer.drag_start_ptr_y = py;
                seat->pointer.drag_start_win_x = hit_view->x;
                seat->pointer.drag_start_win_y = hit_view->y;
                return;  // don't forward click to client during drag start
            }
        }
    }

    if (state == LIBINPUT_BUTTON_STATE_RELEASED && seat->pointer.dragging_view) {
        seat->pointer.dragging_view = NULL;
        return;
    }

    // Forward button to focused surface
    triangles_pointer_send_button(seat, button, state);
}

// ─── Pointer axis (scroll) ───────────────────────────────────────────────────

static void handle_pointer_axis(struct triangles_compositor *compositor,
                                  struct libinput_event_pointer *event) {
    struct triangles_seat *seat = get_seat(compositor);
    if (!seat) return;

    if (libinput_event_pointer_has_axis(event,
            LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
        double v = libinput_event_pointer_get_axis_value(event,
                       LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
        triangles_pointer_send_axis(seat, WL_POINTER_AXIS_VERTICAL_SCROLL, v);
    }
    if (libinput_event_pointer_has_axis(event,
            LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)) {
        double v = libinput_event_pointer_get_axis_value(event,
                       LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
        triangles_pointer_send_axis(seat, WL_POINTER_AXIS_HORIZONTAL_SCROLL, v);
    }
}

// ─── Keyboard ────────────────────────────────────────────────────────────────

static void handle_keyboard_key(struct triangles_compositor *compositor,
                                  struct libinput_event_keyboard *event) {
    struct triangles_seat *seat = get_seat(compositor);
    if (!seat || !seat->keyboard.state) return;

    uint32_t keycode = libinput_event_keyboard_get_key(event);
    enum libinput_key_state state = libinput_event_keyboard_get_key_state(event);

    // XKB uses evdev keycode + 8
    xkb_keycode_t xkb_keycode = keycode + 8;
    xkb_state_update_key(seat->keyboard.state, xkb_keycode,
                         state == LIBINPUT_KEY_STATE_PRESSED
                         ? XKB_KEY_DOWN : XKB_KEY_UP);

    xkb_keysym_t keysym = xkb_state_key_get_one_sym(seat->keyboard.state,
                                                      xkb_keycode);

    // Compositor shortcuts take priority — never forwarded to clients
    if (state == LIBINPUT_KEY_STATE_PRESSED) {
        bool ctrl = xkb_state_mod_name_is_active(seat->keyboard.state,
                        XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE);
        bool alt  = xkb_state_mod_name_is_active(seat->keyboard.state,
                        XKB_MOD_NAME_ALT,  XKB_STATE_MODS_EFFECTIVE);

        if (keysym == XKB_KEY_BackSpace && ctrl && alt) {
            printf("Ctrl+Alt+Backspace — shutting down\n");
            compositor->running = false;
            return;
        }
    }

    // Forward to the keyboard-focused client
    triangles_keyboard_send_key(seat, keycode, state);
}

// ─── Public init ─────────────────────────────────────────────────────────────

bool triangles_input_init(struct triangles_compositor *compositor) {
    // libseat
    compositor->seat = libseat_open_seat(&seat_listener, compositor);
    if (!compositor->seat) {
        fprintf(stderr, "Failed to open seat\n");
        return false;
    }
    printf("Libseat opened\n");

    int seat_fd = libseat_get_fd(compositor->seat);
    if (!wl_event_loop_add_fd(compositor->event_loop, seat_fd,
                               WL_EVENT_READABLE,
                               libseat_source_dispatch, compositor)) {
        fprintf(stderr, "Failed to add libseat fd to event loop\n");
        libseat_close_seat(compositor->seat);
        return false;
    }
    while (libseat_dispatch(compositor->seat, 100) > 0);

    // libinput
    compositor->libinput = libinput_udev_create_context(
        &libinput_interface_impl, compositor, compositor->udev);
    if (!compositor->libinput) {
        fprintf(stderr, "Failed to create libinput context\n");
        libseat_close_seat(compositor->seat);
        return false;
    }

    const char *seat_name = libseat_seat_name(compositor->seat);
    if (!seat_name) seat_name = "seat0";

    if (libinput_udev_assign_seat(compositor->libinput, seat_name) != 0) {
        fprintf(stderr, "Failed to assign seat to libinput\n");
        libinput_unref(compositor->libinput);
        libseat_close_seat(compositor->seat);
        compositor->libinput = NULL;
        return false;
    }

    int li_fd = libinput_get_fd(compositor->libinput);
    if (!wl_event_loop_add_fd(compositor->event_loop, li_fd,
                               WL_EVENT_READABLE,
                               libinput_source_dispatch, compositor)) {
        fprintf(stderr, "Failed to add libinput fd to event loop\n");
        libinput_unref(compositor->libinput);
        libseat_close_seat(compositor->seat);
        compositor->libinput = NULL;
        return false;
    }

    // Set up XKB keymap on the seat
    if (!wl_list_empty(&compositor->seat_list)) {
        struct triangles_seat *seat = wl_container_of(
            compositor->seat_list.next, seat, link);

        seat->keyboard.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (seat->keyboard.context) {
            struct xkb_rule_names names = { .layout = "us" };
            seat->keyboard.keymap = xkb_keymap_new_from_names(
                seat->keyboard.context, &names,
                XKB_KEYMAP_COMPILE_NO_FLAGS);
            if (seat->keyboard.keymap) {
                seat->keyboard.state = xkb_state_new(seat->keyboard.keymap);
                triangles_seat_update_keymap(seat);
            }
        }

        // Center pointer on the primary output so cursor is visible immediately
        if (!wl_list_empty(&compositor->output_list)) {
            struct triangles_output *output = wl_container_of(
                compositor->output_list.next, output, link);
            seat->pointer.x = output->width  / 2.0;
            seat->pointer.y = output->height / 2.0;
            // Repaint now so the cursor appears without needing mouse movement
            triangles_output_repaint(output);
        }
    }

    printf("Input system initialized\n");
    return true;
}

// ─── Public event dispatcher ─────────────────────────────────────────────────

void triangles_input_handle_event(struct triangles_compositor *compositor,
                                   struct libinput_event *event) {
    enum libinput_event_type type = libinput_event_get_type(event);
    switch (type) {
        case LIBINPUT_EVENT_DEVICE_ADDED:
            printf("Input device: %s\n",
                   libinput_device_get_name(libinput_event_get_device(event)));
            break;
        case LIBINPUT_EVENT_DEVICE_REMOVED:
            printf("Input device removed: %s\n",
                   libinput_device_get_name(libinput_event_get_device(event)));
            break;
        case LIBINPUT_EVENT_POINTER_MOTION:
            handle_pointer_motion(compositor,
                libinput_event_get_pointer_event(event));
            break;
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
            handle_pointer_motion_absolute(compositor,
                libinput_event_get_pointer_event(event));
            break;
        case LIBINPUT_EVENT_POINTER_BUTTON:
            handle_pointer_button(compositor,
                libinput_event_get_pointer_event(event));
            break;
        case LIBINPUT_EVENT_POINTER_AXIS:
            handle_pointer_axis(compositor,
                libinput_event_get_pointer_event(event));
            break;
        case LIBINPUT_EVENT_KEYBOARD_KEY:
            handle_keyboard_key(compositor,
                libinput_event_get_keyboard_event(event));
            break;
        default:
            break;
    }
}

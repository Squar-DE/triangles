// main.c - Triangles Wayland Compositor
// Main compositor loop and initialization

#include "compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static struct triangles_compositor *compositor = NULL;

static void signal_handler(int signum) {
    if (compositor) {
        compositor->running = false;
    }
}

int main(int argc, char *argv[]) {
    printf("=== Triangles Wayland Compositor Debug Build ===\n");
    fflush(stdout);
    
    // Setup signal handlers
    printf("Setting up signal handlers...\n");
    fflush(stdout);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize compositor
    printf("Creating compositor structure...\n");
    fflush(stdout);
    compositor = triangles_compositor_create();
    if (!compositor) {
        fprintf(stderr, "Failed to create compositor\n");
        return 1;
    }
    printf("Compositor structure created\n");
    fflush(stdout);

    printf("Triangles Wayland Compositor starting...\n");
    printf("Focusing on proper fractional scaling and rendering\n");
    fflush(stdout);

    // Initialize all subsystems
    printf("Initializing compositor subsystems...\n");
    fflush(stdout);
    if (!triangles_compositor_init(compositor)) {
        fprintf(stderr, "Failed to initialize compositor\n");
        triangles_compositor_destroy(compositor);
        return 1;
    }
    printf("Compositor subsystems initialized\n");
    fflush(stdout);

    // Set the wayland display socket
    printf("Creating Wayland socket...\n");
    fflush(stdout);
    const char *socket = wl_display_add_socket_auto(compositor->display);
    if (!socket) {
        fprintf(stderr, "Failed to create Wayland socket\n");
        triangles_compositor_destroy(compositor);
        return 1;
    }

    printf("Wayland display socket: %s\n", socket);
    setenv("WAYLAND_DISPLAY", socket, 1);
    printf("WAYLAND_DISPLAY environment variable set\n");
    fflush(stdout);

    // Main compositor loop
    printf("=== Compositor ready, entering main loop ===\n");
    printf("Press Ctrl+Alt+Backspace to quit\n");
    fflush(stdout);

    while (compositor->running) {
        wl_display_flush_clients(compositor->display);
        // Block until an event arrives (input, DRM flip, client request).
        // DRM page-flip callbacks are dispatched here via the drm_fd event
        // source, which schedules the next repaint if needs_repaint is set.
        wl_event_loop_dispatch(compositor->event_loop, -1);

        // Kick off any repaints that were scheduled during this dispatch
        // but couldn't run because a flip was already in flight.
        struct triangles_output *output;
        wl_list_for_each(output, &compositor->output_list, link) {
            if (output->needs_repaint && !output->flip_pending) {
                output->needs_repaint = false;
                triangles_output_repaint(output);
            }
        }
    }

    printf("Shutting down compositor...\n");
    fflush(stdout);
    triangles_compositor_destroy(compositor);
    
    return 0;
}

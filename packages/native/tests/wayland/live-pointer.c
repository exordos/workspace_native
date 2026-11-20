// Drive the runner's private compositor using the wlroots virtual-pointer protocol.
#include "virtual-pointer.h"
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

static struct zwlr_virtual_pointer_manager_v1 *manager;
static void bind_manager(void *unused, struct wl_registry *registry, uint32_t name,
                         const char *interface, uint32_t version)
{
    (void)unused;
    if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name))
        manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface,
                                   version < 2 ? version : 2);
}
static void remove_global(void *unused, struct wl_registry *registry, uint32_t name)
{
    (void)unused; (void)registry; (void)name;
}
static uint32_t now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 2;
    struct wl_registry *registry = wl_display_get_registry(display);
    const struct wl_registry_listener listener = {bind_manager, remove_global};
    wl_registry_add_listener(registry, &listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !manager) return 2;
    struct zwlr_virtual_pointer_v1 *pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
    if (wl_display_roundtrip(display) < 0) return 2;
    puts("ready"); fflush(stdout);
    char line[96], operation[16];
    int a, b;
    while (fgets(line, sizeof(line), stdin)) {
        const int count = sscanf(line, "%15s %d %d", operation, &a, &b);
        const uint32_t t = now();
        if (count == 3 && !strcmp(operation, "move") && a >= 0 && a < 1440 && b >= 0 && b < 900) {
            zwlr_virtual_pointer_v1_motion_absolute(pointer, t, a, b, 1440, 900);
        } else if (count == 1 && !strcmp(operation, "click")) {
            zwlr_virtual_pointer_v1_button(pointer,t,BTN_LEFT,WL_POINTER_BUTTON_STATE_PRESSED);
            zwlr_virtual_pointer_v1_frame(pointer);
            zwlr_virtual_pointer_v1_button(pointer,t,BTN_LEFT,WL_POINTER_BUTTON_STATE_RELEASED);
        } else if (count == 2 && (!strcmp(operation, "wheel") || !strcmp(operation, "finger"))) {
            const bool wheel = !strcmp(operation, "wheel");
            zwlr_virtual_pointer_v1_axis_source(pointer, wheel ? WL_POINTER_AXIS_SOURCE_WHEEL : WL_POINTER_AXIS_SOURCE_FINGER);
            if (wheel)
                zwlr_virtual_pointer_v1_axis_discrete(pointer,t,WL_POINTER_AXIS_VERTICAL_SCROLL,wl_fixed_from_int(a),a > 0 ? 1 : -1);
            else if (a)
                zwlr_virtual_pointer_v1_axis(pointer,t,WL_POINTER_AXIS_VERTICAL_SCROLL,wl_fixed_from_int(a));
            else
                zwlr_virtual_pointer_v1_axis_stop(pointer,t,WL_POINTER_AXIS_VERTICAL_SCROLL);
        } else {
            return 2;
        }
        zwlr_virtual_pointer_v1_frame(pointer);
        if (wl_display_roundtrip(display) < 0) return 2;
        puts("ok"); fflush(stdout);
    }
    zwlr_virtual_pointer_v1_destroy(pointer);
    zwlr_virtual_pointer_manager_v1_destroy(manager);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}

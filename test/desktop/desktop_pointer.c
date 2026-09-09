// SPDX-License-Identifier: LGPL-2.1-or-later
// A persistent, unprivileged Wayland input client for the private test server.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include "virtual-pointer-client.h"

static struct zwlr_virtual_pointer_manager_v1 *manager;
static struct wl_seat *seat;

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, "zwlr_virtual_pointer_manager_v1") == 0)
        manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (strcmp(interface, "wl_seat") == 0 && !seat)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
}

static void removed(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static uint32_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        exit(2);
    return (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display)
        return 2;
    struct wl_registry *registry = wl_display_get_registry(display);
    const struct wl_registry_listener listener = {global, removed};
    wl_registry_add_listener(registry, &listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !manager || !seat)
    {
        fputs("Virtual pointer protocol and seat are required\n", stderr);
        return 2;
    }
    struct zwlr_virtual_pointer_v1 *pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
    if (wl_display_roundtrip(display) < 0)
        return 2;
    puts("READY");
    fflush(stdout);

    bool pressed = false;
    char line[128];
    while (fgets(line, sizeof(line), stdin))
    {
        unsigned int x, y, width, height;
        char extra;
        if (strcmp(line, "release\n") == 0)
        {
            if (!pressed)
                return 2;
            zwlr_virtual_pointer_v1_button(pointer, milliseconds(), 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
            zwlr_virtual_pointer_v1_frame(pointer);
            pressed = false;
        }
        else
        {
            if (pressed || sscanf(line, "press %u %u %u %u %c", &x, &y, &width, &height, &extra) != 4
                || !width || !height || x >= width || y >= height)
                return 2;
            zwlr_virtual_pointer_v1_motion_absolute(pointer, milliseconds(), x, y, width, height);
            zwlr_virtual_pointer_v1_frame(pointer);
            if (wl_display_roundtrip(display) < 0)
                return 2;
            zwlr_virtual_pointer_v1_button(pointer, milliseconds(), 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
            zwlr_virtual_pointer_v1_frame(pointer);
            pressed = true;
        }
        if (wl_display_roundtrip(display) < 0)
            return 2;
        puts("OK");
        fflush(stdout);
    }
    if (pressed)
    {
        zwlr_virtual_pointer_v1_button(pointer, milliseconds(), 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
        zwlr_virtual_pointer_v1_frame(pointer);
    }
    zwlr_virtual_pointer_v1_destroy(pointer);
    zwlr_virtual_pointer_manager_v1_destroy(manager);
    wl_seat_destroy(seat);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
    return 0;
}

// SPDX-License-Identifier: LGPL-2.1-or-later
// Keep one native keyboard and one keymap for the private desktop test session.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-client.h"

static struct zwp_virtual_keyboard_manager_v1 *manager;
static struct wl_seat *seat;

static void fail(const char *message)
{
    fprintf(stderr, "desktop_keyboard: %s\n", message);
    exit(2);
}

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, "zwp_virtual_keyboard_manager_v1") == 0)
        manager = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
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
        fail("Cannot read the monotonic clock");
    return (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static void send_key(struct wl_display *display, struct zwp_virtual_keyboard_v1 *keyboard,
                     struct xkb_keymap *keymap, xkb_keysym_t symbol, uint32_t modifiers)
{
    xkb_keycode_t found = XKB_KEYCODE_INVALID;
    for (xkb_keycode_t code = xkb_keymap_min_keycode(keymap);
         code <= xkb_keymap_max_keycode(keymap); ++code)
    {
        const xkb_keysym_t *symbols = NULL;
        if (xkb_keymap_key_get_syms_by_level(keymap, code, 0, 0, &symbols) == 1
            && symbols[0] == symbol)
        {
            found = code;
            break;
        }
    }
    if (symbol == XKB_KEY_NoSymbol || found == XKB_KEYCODE_INVALID || found < 8)
        fail("Requested key is not in the test keyboard's unshifted US layout");
    zwp_virtual_keyboard_v1_modifiers(keyboard, modifiers, 0, 0, 0);
    // XKB keycodes include an offset of eight relative to Wayland/evdev codes.
    zwp_virtual_keyboard_v1_key(keyboard, milliseconds(), found - 8, WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key(keyboard, milliseconds(), found - 8, WL_KEYBOARD_KEY_STATE_RELEASED);
    zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
    if (wl_display_roundtrip(display) < 0)
        fail("Keyboard connection failed");
}

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display)
        fail("Cannot connect to the private Wayland server");
    struct wl_registry *registry = wl_display_get_registry(display);
    const struct wl_registry_listener listener = {global, removed};
    wl_registry_add_listener(registry, &listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !manager || !seat)
        fail("Virtual keyboard protocol and seat are required");

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context)
        fail("Cannot create the XKB context");
    const struct xkb_rule_names names = {.layout = "us"};
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap)
        fail("Cannot compile the test keymap");
    char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!text || strlen(text) >= UINT32_MAX)
        fail("Cannot serialize the test keymap");
    char filename[] = "/tmp/lxqt-desktop-keymap-XXXXXX";
    int fd = mkstemp(filename);
    if (fd < 0)
        fail("Cannot create the private keymap file");
    unlink(filename);
    FILE *file = fdopen(fd, "w+");
    if (!file)
        fail("Cannot open the private keymap stream");
    size_t size = strlen(text) + 1;
    if (fwrite(text, 1, size, file) != size || fflush(file) != 0)
        fail("Cannot write the test keymap");
    free(text);

    struct zwp_virtual_keyboard_v1 *keyboard =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(manager, seat);
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    if (wl_display_roundtrip(display) < 0)
        fail("Cannot install the test keymap");
    fclose(file);
    xkb_mod_index_t alt = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);
    if (alt == XKB_MOD_INVALID || alt >= 32)
        fail("The test keymap has no usable Alt modifier");
    puts("READY");
    fflush(stdout);

    char line[1024];
    while (fgets(line, sizeof(line), stdin))
    {
        size_t length = strlen(line);
        if (!length || line[length - 1] != '\n')
            fail("Expected one bounded, newline-terminated command");
        line[length - 1] = '\0';
        if (strncmp(line, "key ", 4) == 0)
        {
            const char *name = line + 4;
            uint32_t modifiers = 0;
            if (strncmp(name, "alt+", 4) == 0)
            {
                modifiers = (uint32_t)1 << alt;
                name += 4;
            }
            send_key(display, keyboard, keymap,
                     xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS), modifiers);
        }
        else if (strncmp(line, "text ", 5) == 0)
        {
            // Test strings are plain ASCII. Unsupported input fails explicitly.
            for (const unsigned char *p = (unsigned char *)line + 5; *p; ++p)
            {
                if (*p < 32 || *p > 126)
                    fail("Only printable ASCII test input is supported");
                send_key(display, keyboard, keymap, xkb_utf32_to_keysym(*p), 0);
            }
        }
        else
            fail("Unknown keyboard command");
        puts("OK");
        fflush(stdout);
    }
    zwp_virtual_keyboard_v1_destroy(keyboard);
    zwp_virtual_keyboard_manager_v1_destroy(manager);
    wl_seat_destroy(seat);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    return 0;
}

#ifndef XBEMU_RAWINPUT_H
#define XBEMU_RAWINPUT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Raw input from XInput2 (raw motion and buttons, raw keys), for windows that registered
 * for it with RegisterRawInputDevices. Each event becomes a RAWINPUT record in a ring of
 * buffers; the handle is the record's address.
 */

#define RAWINPUT_KIND_MOUSE    1u
#define RAWINPUT_KIND_KEYBOARD 2u

/* Register (or, with remove, unregister) a window for a kind of raw input. */
void rawinput_set_target(uint64_t hwnd, unsigned kind, int remove);

/* Translates an X keycode to a Windows virtual-key code (supplied by user32). */
typedef uint8_t (*rawinput_vk_fn)(unsigned keycode);

/* Read pending raw events. Calls deliver(hwnd, handle) for each WM_INPUT to post. */
typedef void (*rawinput_deliver_fn)(uint64_t hwnd, uint64_t handle);
void rawinput_poll(rawinput_deliver_fn deliver, rawinput_vk_fn vk_of_keycode);

/* GetRawInputData for a handle returned by rawinput_poll. Returns the size, or -1. */
uint32_t rawinput_get_data(uint64_t handle, uint32_t command, void *data, uint32_t *size, uint32_t header_size);

#endif

#ifndef XBEMU_USER32_IMPL_H
#define XBEMU_USER32_IMPL_H

#include <stdint.h>
#include <xcb/xcb.h>

/*
 * For the Vulkan shim: the X11 window behind an HWND.
 * Returns 1 and sets *out if hwnd is a live window, 0 otherwise.
 */
int xbemu_hwnd_to_xcb(uint64_t hwnd, xcb_window_t *out);

#endif

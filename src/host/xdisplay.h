#ifndef XBEMU_XDISPLAY_H
#define XBEMU_XDISPLAY_H

#include <stdint.h>
#include <xcb/xcb.h>

/* Shared X11 connection (xcb), opened on first use from $DISPLAY. NULL if no display. */
xcb_connection_t *xbemu_xcb_connection(void);

/*
 * A separate connection for Vulkan surfaces. Mesa's Present machinery reads its own
 * events from the surface connection, so window event handling must not share it.
 */
xcb_connection_t *xbemu_xcb_surface_connection(void);

/* Default screen of that connection. Valid only after xbemu_xcb_connection() succeeds. */
xcb_screen_t *xbemu_xcb_screen(void);

#endif

/*
 * Shared X11 connection for window and surface code (user32 and the Vulkan shim).
 * Opened lazily so programs that never show a window don't need a display.
 */
#define _DEFAULT_SOURCE
#include "xdisplay.h"

#include <stdio.h>
#include <stdlib.h>

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static int tried;

xcb_connection_t *xbemu_xcb_connection(void) {
    if (!tried) {
        tried = 1;
        int scr = 0;
        conn = xcb_connect(NULL, &scr);
        if (xcb_connection_has_error(conn)) {
            fprintf(stderr, "[xbemu] cannot connect to X display (DISPLAY=%s)\n",
                    getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
            xcb_disconnect(conn);
            conn = NULL;
            return NULL;
        }
        const xcb_setup_t *setup = xcb_get_setup(conn);
        xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
        for (int i = 0; i < scr && it.rem; i++) xcb_screen_next(&it);
        screen = it.data;
    }
    return conn;
}

xcb_screen_t *xbemu_xcb_screen(void) {
    return screen;
}

xcb_connection_t *xbemu_xcb_surface_connection(void) {
    static xcb_connection_t *surface_conn;
    static int surface_tried;
    if (!surface_tried) {
        surface_tried = 1;
        int scr = 0;
        surface_conn = xcb_connect(NULL, &scr);
        if (xcb_connection_has_error(surface_conn)) {
            xcb_disconnect(surface_conn);
            surface_conn = NULL;
        }
    }
    return surface_conn;
}

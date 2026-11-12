/*
 * Test helper: sends X key and mouse events to the input test window, found by its title.
 * Usage: sendevents "xbemu input test"
 * The events go through the X server, as real input would. The window receives them
 * because it selected the matching event masks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>

static xcb_window_t find_window(xcb_connection_t *c, xcb_window_t root, const char *title) {
    xcb_query_tree_reply_t *t = xcb_query_tree_reply(c, xcb_query_tree(c, root), NULL);
    if (!t) return 0;
    xcb_window_t *kids = xcb_query_tree_children(t);
    xcb_window_t found = 0;
    for (int i = 0; i < xcb_query_tree_children_length(t) && !found; i++) {
        xcb_get_property_reply_t *p = xcb_get_property_reply(c,
            xcb_get_property(c, 0, kids[i], XCB_ATOM_WM_NAME, XCB_ATOM_ANY, 0, 256), NULL);
        if (p && xcb_get_property_value_length(p) > 0 &&
            strncmp((const char *)xcb_get_property_value(p), title, xcb_get_property_value_length(p)) == 0 &&
            strlen(title) == (size_t)xcb_get_property_value_length(p))
            found = kids[i];
        free(p);
    }
    free(t);
    return found;
}

/* Keycode for an X keysym on this server (level 0), via the core keyboard mapping. */
static xcb_keycode_t keycode_for(xcb_connection_t *c, xcb_keysym_t want) {
    const xcb_setup_t *s = xcb_get_setup(c);
    uint8_t count = s->max_keycode - s->min_keycode + 1;
    xcb_get_keyboard_mapping_reply_t *r =
        xcb_get_keyboard_mapping_reply(c, xcb_get_keyboard_mapping(c, s->min_keycode, count), NULL);
    xcb_keycode_t code = 0;
    xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(r);
    for (int i = 0; i < count && !code; i++)
        if (syms[i * r->keysyms_per_keycode] == want) code = (xcb_keycode_t)(s->min_keycode + i);
    free(r);
    return code;
}

/* state: the modifier and lock mask the event carries (0x02 = caps lock). */
static void send_key_state(xcb_connection_t *c, xcb_window_t w, int press, xcb_keycode_t code, uint16_t state) {
    xcb_key_press_event_t e = { 0 };
    e.response_type = press ? XCB_KEY_PRESS : XCB_KEY_RELEASE;
    e.detail = code;
    e.state = state;
    e.event = w;
    e.root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    xcb_send_event(c, 0, w, XCB_EVENT_MASK_NO_EVENT, (const char *)&e);
}

static void send_key(xcb_connection_t *c, xcb_window_t w, int press, xcb_keycode_t code) {
    send_key_state(c, w, press, code, 0);
}

static void send_button(xcb_connection_t *c, xcb_window_t w, int press, uint8_t button, int16_t x, int16_t y) {
    xcb_button_press_event_t e = { 0 };
    e.response_type = press ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE;
    e.detail = button;
    e.event = w;
    e.event_x = x;
    e.event_y = y;
    xcb_send_event(c, 0, w, XCB_EVENT_MASK_NO_EVENT, (const char *)&e);
}

static void send_motion(xcb_connection_t *c, xcb_window_t w, int16_t x, int16_t y) {
    xcb_motion_notify_event_t e = { 0 };
    e.response_type = XCB_MOTION_NOTIFY;
    e.event = w;
    e.event_x = x;
    e.event_y = y;
    xcb_send_event(c, 0, w, XCB_EVENT_MASK_NO_EVENT, (const char *)&e);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: sendevents <window title>\n"); return 2; }
    int scr;
    xcb_connection_t *c = xcb_connect(NULL, &scr);
    if (xcb_connection_has_error(c)) return 1;
    xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    xcb_window_t w = 0;
    for (int tries = 0; tries < 100 && !w; tries++) {
        w = find_window(c, root, argv[1]);
        if (!w) usleep(100000);
    }
    if (!w) { fprintf(stderr, "window not found\n"); return 1; }

    usleep(300000);
    xcb_keycode_t a = keycode_for(c, XK_a);
    xcb_keycode_t esc = keycode_for(c, XK_Escape);
    xcb_keycode_t caps = keycode_for(c, XK_Caps_Lock);
    send_key(c, w, 1, a); xcb_flush(c); usleep(50000);
    send_key(c, w, 0, a); xcb_flush(c); usleep(50000);
    /* Caps lock: tap it, then type 'a' with the lock state set. Expect an uppercase 'A'. */
    send_key(c, w, 1, caps); xcb_flush(c); usleep(50000);
    send_key(c, w, 0, caps); xcb_flush(c); usleep(50000);
    send_key_state(c, w, 1, a, 0x02); xcb_flush(c); usleep(50000);
    send_key_state(c, w, 0, a, 0x02); xcb_flush(c); usleep(50000);
    send_button(c, w, 1, 1, 10, 20); xcb_flush(c); usleep(50000);
    send_motion(c, w, 30, 40); xcb_flush(c); usleep(50000);
    send_button(c, w, 0, 1, 30, 40); xcb_flush(c); usleep(50000);
    send_button(c, w, 1, 4, 5, 5); xcb_flush(c); usleep(50000);  /* wheel up */
    send_key(c, w, 1, esc); xcb_flush(c);
    usleep(200000);
    return 0;
}

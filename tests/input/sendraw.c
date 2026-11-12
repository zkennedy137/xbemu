/*
 * Test helper: sends synthetic input through XTest. Each event is reported as raw input,
 * the same as from a real device:
 *   relative motion (7, -3), left button press and release, key 'a' press and release.
 */
#include <stdio.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

static void step(Display *d) {
    XFlush(d);
    usleep(80000);
}

int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    usleep(400000);   /* let the test program register and print "ready" */
    XTestFakeRelativeMotionEvent(d, 7, -3, 0); step(d);
    XTestFakeButtonEvent(d, 1, True, 0); step(d);
    XTestFakeButtonEvent(d, 1, False, 0); step(d);
    KeyCode a = XKeysymToKeycode(d, XK_a);
    XTestFakeKeyEvent(d, a, True, 0); step(d);
    XTestFakeKeyEvent(d, a, False, 0); step(d);
    XCloseDisplay(d);
    return 0;
}

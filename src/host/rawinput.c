/*
 * Raw input (XInput2 on a separate Xlib connection): relative mouse motion, mouse buttons,
 * and keys. Each event is a RAWINPUT record (48 bytes on x64) in a ring of buffers.
 *
 * Layout (verified against the MinGW headers):
 *   RAWINPUTHEADER at 0: dwType 0 (mouse) or 1 (keyboard), dwSize, hDevice 8, wParam 16.
 *   RAWMOUSE at 24: usFlags 0, usButtonFlags 28, usButtonData 30, lLastX 36, lLastY 40.
 *   RAWKEYBOARD at 24: MakeCode 24, Flags 26 (RI_KEY_BREAK = 1 on release), VKey 30,
 *                      Message 32.
 *
 * Not implemented: absolute devices, the wheel's raw data, and device enumeration.
 */
#define _DEFAULT_SOURCE
#include "rawinput.h"

#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#define RAW_TARGETS_MAX 32
#define RAW_RING 128
#define RAWINPUT_SIZE 48
#define RID_INPUT  0x10000003u
#define RID_HEADER 0x10000005u
#define RIM_TYPEMOUSE    0u
#define RIM_TYPEKEYBOARD 1u
#define RI_KEY_BREAK 1u
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP   0x0101u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSKEYUP   0x0105u
#define RI_MOUSE_LEFT_BUTTON_DOWN   0x0001u
#define RI_MOUSE_LEFT_BUTTON_UP     0x0002u
#define RI_MOUSE_RIGHT_BUTTON_DOWN  0x0004u
#define RI_MOUSE_RIGHT_BUTTON_UP    0x0008u
#define RI_MOUSE_MIDDLE_BUTTON_DOWN 0x0010u
#define RI_MOUSE_MIDDLE_BUTTON_UP   0x0020u

typedef struct Target { uint64_t hwnd; unsigned kind; } Target;
static Target targets[RAW_TARGETS_MAX];
static int ntargets;
static Display *dpy;
static int xi_opcode;
static int dpy_tried;
static uint8_t ring[RAW_RING][RAWINPUT_SIZE];
static unsigned ring_next;

static int open_input(void) {
    if (dpy_tried) return dpy != NULL;
    dpy_tried = 1;
    dpy = XOpenDisplay(NULL);
    if (!dpy) return 0;
    int event, error;
    if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &event, &error)) return 0;
    int major = 2, minor = 0;
    if (XIQueryVersion(dpy, &major, &minor) != Success) return 0;
    unsigned char mask[XIMaskLen(XI_LASTEVENT)];
    memset(mask, 0, sizeof mask);
    XISetMask(mask, XI_RawMotion);
    XISetMask(mask, XI_RawButtonPress);
    XISetMask(mask, XI_RawButtonRelease);
    XISetMask(mask, XI_RawKeyPress);
    XISetMask(mask, XI_RawKeyRelease);
    XIEventMask em = { XIAllDevices, sizeof mask, mask };
    XISelectEvents(dpy, DefaultRootWindow(dpy), &em, 1);
    XFlush(dpy);
    return 1;
}

void rawinput_set_target(uint64_t hwnd, unsigned kind, int remove) {
    if (!hwnd) return;
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].hwnd == hwnd && targets[i].kind == kind) {
            if (remove) targets[i] = targets[--ntargets];
            return;
        }
    }
    if (!remove && ntargets < RAW_TARGETS_MAX) {
        targets[ntargets].hwnd = hwnd;
        targets[ntargets].kind = kind;
        ntargets++;
        open_input();
    }
}

static uint8_t *new_record(uint32_t type) {
    uint8_t *r = ring[ring_next];
    ring_next = (ring_next + 1) % RAW_RING;
    memset(r, 0, RAWINPUT_SIZE);
    uint32_t size = RAWINPUT_SIZE;
    uint64_t hdev = type == RIM_TYPEMOUSE ? 1 : 2;   /* one mouse, one keyboard */
    memcpy(r + 0, &type, 4);
    memcpy(r + 4, &size, 4);
    memcpy(r + 8, &hdev, 8);
    return r;
}

static uint64_t mouse_record(int32_t dx, int32_t dy, uint16_t button_flags, uint16_t button_data) {
    uint8_t *r = new_record(RIM_TYPEMOUSE);
    memcpy(r + 28, &button_flags, 2);
    memcpy(r + 30, &button_data, 2);
    memcpy(r + 36, &dx, 4);
    memcpy(r + 40, &dy, 4);
    return (uint64_t)(uintptr_t)r;
}

static uint64_t key_record(uint16_t scan, uint16_t flags, uint16_t vkey, uint32_t message) {
    uint8_t *r = new_record(RIM_TYPEKEYBOARD);
    memcpy(r + 24, &scan, 2);
    memcpy(r + 26, &flags, 2);
    memcpy(r + 30, &vkey, 2);
    memcpy(r + 32, &message, 4);
    return (uint64_t)(uintptr_t)r;
}

/* The server can report one raw event more than once (the same time and details). The last
 * event is remembered so its repeat is dropped. */
static Time last_time;
static int last_evtype = -1, last_detail = -1;
static double last_value;

static int is_repeat(XIRawEvent *re, int evtype) {
    double first = 0;
    for (int a = 0; a < 2; a++) {
        if (XIMaskIsSet(re->valuators.mask, a)) { first = re->raw_values[0]; break; }
    }
    int repeat = re->time == last_time && evtype == last_evtype && re->detail == last_detail &&
                 first == last_value;
    last_time = re->time;
    last_evtype = evtype;
    last_detail = re->detail;
    last_value = first;
    return repeat;
}

static void send(rawinput_deliver_fn deliver, unsigned kind, uint64_t handle) {
    for (int i = 0; i < ntargets; i++)
        if (targets[i].kind == kind && deliver) deliver(targets[i].hwnd, handle);
}

static int axis_value(XIRawEvent *re, int axis, int32_t *out) {
    int k = 0;
    for (int a = 0; a < axis; a++) if (XIMaskIsSet(re->valuators.mask, a)) k++;
    if (!XIMaskIsSet(re->valuators.mask, axis)) return 0;
    *out = (int32_t)re->raw_values[k];
    return 1;
}

void rawinput_poll(rawinput_deliver_fn deliver, rawinput_vk_fn vk_of_keycode) {
    if (!dpy) return;
    while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.xcookie.type != GenericEvent || ev.xcookie.extension != xi_opcode) continue;
        if (!XGetEventData(dpy, &ev.xcookie)) continue;
        XIRawEvent *re = (XIRawEvent *)ev.xcookie.data;
        if (is_repeat(re, ev.xcookie.evtype)) {
            XFreeEventData(dpy, &ev.xcookie);
            continue;
        }
        switch (ev.xcookie.evtype) {
        case XI_RawMotion: {
            int32_t dx = 0, dy = 0;
            axis_value(re, 0, &dx);
            axis_value(re, 1, &dy);
            if (dx || dy) send(deliver, RAWINPUT_KIND_MOUSE, mouse_record(dx, dy, 0, 0));
            break;
        }
        case XI_RawButtonPress:
        case XI_RawButtonRelease: {
            int down = ev.xcookie.evtype == XI_RawButtonPress;
            uint16_t flags = 0, data = 0;
            switch (re->detail) {
            case 1: flags = down ? RI_MOUSE_LEFT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_UP; break;
            case 2: flags = down ? RI_MOUSE_MIDDLE_BUTTON_DOWN : RI_MOUSE_MIDDLE_BUTTON_UP; break;
            case 3: flags = down ? RI_MOUSE_RIGHT_BUTTON_DOWN : RI_MOUSE_RIGHT_BUTTON_UP; break;
            default: break;   /* wheel and extra buttons are not sent */
            }
            if (flags) send(deliver, RAWINPUT_KIND_MOUSE, mouse_record(0, 0, flags, data));
            break;
        }
        case XI_RawKeyPress:
        case XI_RawKeyRelease: {
            int down = ev.xcookie.evtype == XI_RawKeyPress;
            uint16_t vk = vk_of_keycode ? vk_of_keycode((unsigned)re->detail) : 0;
            if (vk) {
                uint16_t scan = (uint16_t)(re->detail - 8);
                uint16_t flags = down ? 0 : RI_KEY_BREAK;
                uint32_t msg = down ? WM_KEYDOWN : WM_KEYUP;
                send(deliver, RAWINPUT_KIND_KEYBOARD, key_record(scan, flags, vk, msg));
            }
            break;
        }
        default:
            break;
        }
        XFreeEventData(dpy, &ev.xcookie);
    }
}

uint32_t rawinput_get_data(uint64_t handle, uint32_t command, void *data, uint32_t *size, uint32_t header_size) {
    (void)header_size;
    const uint8_t *src = (const uint8_t *)(uintptr_t)handle;
    if (!src || !size) return (uint32_t)-1;
    uint32_t want;
    if (command == RID_INPUT) want = RAWINPUT_SIZE;
    else if (command == RID_HEADER) want = 24;
    else return (uint32_t)-1;
    if (!data) { *size = want; return 0; }
    if (*size < want) { *size = want; return (uint32_t)-1; }
    memcpy(data, src, want);
    return want;
}

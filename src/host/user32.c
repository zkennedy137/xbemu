/*
 * user32.dll shims: window classes, windows, and the message loop.
 *
 * Each HWND is a host object backed by an X11 window (through xcb). X events become
 * Win32 messages in a queue. Window procedures are guest functions, called with the
 * Win64 convention through xbemu_call_w64.
 *
 * Simplifications: no keyboard or mouse messages yet, no non-client area (so
 * AdjustWindowRectEx is a no-op), no display mode changes, and only one monitor.
 */
#define _DEFAULT_SOURCE
#include "kernel32_impl.h"
#include "xdisplay.h"
#include "display.h"
#include "rawinput.h"
#include "user32_impl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>

/* ---- constants (Win32 values) ---- */

#define WM_CREATE          0x0001
#define WM_SIZE            0x0005
#define WM_PAINT           0x000F
#define WM_CLOSE           0x0010
#define WM_DESTROY         0x0002
#define WM_QUIT            0x0012
#define WS_VISIBLE         0x10000000u
#define CW_USEDEFAULT_VAL  0x80000000u
#define PM_REMOVE          0x0001
#define SW_HIDE            0
#define SWP_NOSIZE         0x0001
#define SWP_NOMOVE         0x0002
#define GWL_WNDPROC        (-4)
#define GWL_STYLE          (-16)
#define GWL_EXSTYLE        (-20)
#define GWLP_USERDATA      (-21)
#define SM_CXSCREEN        0
#define SM_CYSCREEN        1
#define WM_INPUT           0x00FF
#define WM_KEYDOWN         0x0100
#define WM_KEYUP           0x0101
#define WM_CHAR            0x0102
#define WM_MOUSEMOVE       0x0200
#define WM_LBUTTONDOWN     0x0201
#define WM_LBUTTONUP       0x0202
#define WM_RBUTTONDOWN     0x0204
#define WM_RBUTTONUP       0x0205
#define WM_MBUTTONDOWN     0x0207
#define WM_MBUTTONUP       0x0208
#define WM_MOUSEWHEEL      0x020A
#define WM_SETFOCUS        0x0007
#define WM_KILLFOCUS       0x0008
#define MK_LBUTTON         0x0001
#define MK_RBUTTON         0x0002
#define MK_SHIFT           0x0004
#define MK_CONTROL         0x0008
#define MK_MBUTTON         0x0010
#define WHEEL_DELTA        120
#define VK_SHIFT_CODE      0x10
#define VK_CONTROL_CODE    0x11
#define VK_MENU_CODE       0x12
#define VK_LBUTTON_CODE    0x01
#define VK_RBUTTON_CODE    0x02
#define VK_MBUTTON_CODE    0x04
#define ERROR_NOT_SUPPORTED 50u
#define DISP_CHANGE_FAILED (-1)

/* ---- objects ---- */

#define MAX_WINDOWS 256
#define MAX_CLASSES 64
#define MSG_QUEUE   256

typedef struct Win {
    xcb_window_t xw;
    uint64_t wndproc;   /* guest WNDPROC */
    uint64_t userdata;  /* GWLP_USERDATA */
    uint32_t style;
    int32_t x, y, w, h;
    int alive;
} Win;

typedef struct WndClass {
    char name[128];      /* class name, ASCII */
    uint64_t wndproc;
    uint16_t atom;
} WndClass;

/* Same layout as the Win32 MSG structure (48 bytes on x64). */
typedef struct MsgRec {
    uint64_t hwnd;
    uint32_t message;
    uint32_t pad0;
    uint64_t wParam;
    uint64_t lParam;
    uint32_t time;
    int32_t pt_x, pt_y;
    uint32_t pad1;
} MsgRec;

static Win *windows[MAX_WINDOWS];
static WndClass classes[MAX_CLASSES];
static size_t nclasses;
static MsgRec queue[MSG_QUEUE];
static size_t q_head, q_count;

static xcb_atom_t atom_wm_protocols, atom_wm_delete, atom_wm_name, atom_utf8;
static int atoms_ready;

/* ---- keyboard and mouse state ---- */

static uint8_t key_down[256];          /* by Windows virtual-key code */
static uint8_t key_toggle[256];        /* toggle keys (caps, num, scroll lock): on/off */
static xcb_keysym_t *keymap;           /* X keysyms: keycode - min_kc, per level */
static uint8_t keymap_min, keymap_per;
static int keymap_ready;
static uint64_t focus_hwnd;

/* Virtual-key code for an X keysym, or 0 when there is no mapping. */
static uint8_t vk_for_keysym(xcb_keysym_t ks) {
    if (ks >= XK_a && ks <= XK_z) return (uint8_t)(ks - XK_a + 'A');
    if (ks >= XK_A && ks <= XK_Z) return (uint8_t)ks;
    if (ks >= XK_0 && ks <= XK_9) return (uint8_t)ks;
    if (ks >= XK_F1 && ks <= XK_F12) return (uint8_t)(0x70 + (ks - XK_F1));
    if (ks >= XK_KP_0 && ks <= XK_KP_9) return (uint8_t)(0x60 + (ks - XK_KP_0));
    switch (ks) {
    case XK_space:        return 0x20;
    case XK_BackSpace:    return 0x08;
    case XK_Tab:          return 0x09;
    case XK_Return:       return 0x0D;
    case XK_KP_Enter:     return 0x0D;
    case XK_Escape:       return 0x1B;
    case XK_Left:         return 0x25;
    case XK_Up:           return 0x26;
    case XK_Right:        return 0x27;
    case XK_Down:         return 0x28;
    case XK_Home:         return 0x24;
    case XK_End:          return 0x23;
    case XK_Page_Up:      return 0x21;
    case XK_Page_Down:    return 0x22;
    case XK_Insert:       return 0x2D;
    case XK_Delete:       return 0x2E;
    case XK_Shift_L:      return 0xA0; /* VK_LSHIFT */
    case XK_Shift_R:      return 0xA1; /* VK_RSHIFT */
    case XK_Control_L:    return 0xA2; /* VK_LCONTROL */
    case XK_Control_R:    return 0xA3; /* VK_RCONTROL */
    case XK_Alt_L:        return 0xA4; /* VK_LMENU */
    case XK_Alt_R:        return 0xA5; /* VK_RMENU */
    case XK_minus:        return 0xBD; /* VK_OEM_MINUS */
    case XK_equal:        return 0xBB; /* VK_OEM_PLUS */
    case XK_comma:        return 0xBC;
    case XK_period:       return 0xBE;
    case XK_slash:        return 0xBF;
    case XK_semicolon:    return 0xBA;
    case XK_bracketleft:  return 0xDB;
    case XK_backslash:    return 0xDC;
    case XK_bracketright: return 0xDD;
    case XK_apostrophe:   return 0xDE;
    case XK_grave:        return 0xC0;
    case XK_Caps_Lock:    return 0x14; /* VK_CAPITAL */
    case XK_Num_Lock:     return 0x90; /* VK_NUMLOCK */
    case XK_Scroll_Lock:  return 0x91; /* VK_SCROLL */
    default:              return 0;
    }
}

/* Read the core keyboard mapping once, so key events can be translated. */
static void load_keymap(xcb_connection_t *c) {
    if (keymap_ready) return;
    const xcb_setup_t *setup = xcb_get_setup(c);
    keymap_min = setup->min_keycode;
    uint8_t count = setup->max_keycode - setup->min_keycode + 1;
    xcb_get_keyboard_mapping_reply_t *r =
        xcb_get_keyboard_mapping_reply(c, xcb_get_keyboard_mapping(c, keymap_min, count), NULL);
    if (!r) return;
    keymap_per = r->keysyms_per_keycode;
    xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(r);
    size_t n = (size_t)count * keymap_per;
    keymap = malloc(n * sizeof *keymap);
    if (keymap) memcpy(keymap, syms, n * sizeof *keymap);
    free(r);
    keymap_ready = 1;
}

/* Keysym for an X keycode at shift level 0 or 1 (no group or lock handling). */
static xcb_keysym_t keysym_for(uint8_t keycode, int shifted) {
    if (!keymap || keycode < keymap_min || keymap_per == 0) return 0;
    size_t idx = (size_t)(keycode - keymap_min) * keymap_per;
    if (shifted && keymap_per > 1) idx += 1;
    return keymap[idx];
}

/* Enqueue a message. Drops it if the queue is full. */
static void post(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    if (q_count == MSG_QUEUE) return;
    MsgRec *m = &queue[(q_head + q_count) % MSG_QUEUE];
    memset(m, 0, sizeof *m);
    m->hwnd = hwnd;
    m->message = msg;
    m->wParam = wp;
    m->lParam = lp;
    q_count++;
}

static Win *win_of(uint64_t hwnd) {
    for (size_t i = 0; i < MAX_WINDOWS; i++)
        if (windows[i] == (Win *)(uintptr_t)hwnd) return windows[i];
    return NULL;
}

static Win *win_by_xid(xcb_window_t xid) {
    for (size_t i = 0; i < MAX_WINDOWS; i++)
        if (windows[i] && windows[i]->alive && windows[i]->xw == xid) return windows[i];
    return NULL;
}

static uint64_t default_proc(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);

/* LRESULT: call the window's procedure, or the default one. */
static uint64_t send_msg(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    Win *w = win_of(hwnd);
    if (w && w->wndproc)
        return xbemu_call_w64((void *)(uintptr_t)w->wndproc, hwnd, msg, wp, lp);
    return default_proc(hwnd, msg, wp, lp);
}

/* ---- X11 atoms and events ---- */

static int init_atoms(xcb_connection_t *c) {
    if (atoms_ready) return 1;
    xcb_intern_atom_cookie_t ck[4];
    ck[0] = xcb_intern_atom(c, 0, 12, "WM_PROTOCOLS");
    ck[1] = xcb_intern_atom(c, 0, 16, "WM_DELETE_WINDOW");
    ck[2] = xcb_intern_atom(c, 0, 7, "WM_NAME");
    ck[3] = xcb_intern_atom(c, 0, 11, "UTF8_STRING");
    xcb_intern_atom_reply_t *r[4];
    for (int i = 0; i < 4; i++) r[i] = xcb_intern_atom_reply(c, ck[i], NULL);
    if (!r[0] || !r[1] || !r[2] || !r[3]) return 0;
    atom_wm_protocols = r[0]->atom;
    atom_wm_delete = r[1]->atom;
    atom_wm_name = r[2]->atom;
    atom_utf8 = r[3]->atom;
    for (int i = 0; i < 4; i++) free(r[i]);
    atoms_ready = 1;
    return 1;
}

/* Toggle state comes with every key and mouse event: Lock is caps lock, Mod2 is num lock. */
static void update_toggles(uint16_t state) {
    key_toggle[0x14] = (state & XCB_MOD_MASK_LOCK) ? 1 : 0;
    key_toggle[0x90] = (state & XCB_MOD_MASK_2) ? 1 : 0;
}

/* Shift, control and menu (Alt) are reported both generically and by side. */
static void update_modifiers(void) {
    key_down[0x10] = key_down[0xA0] | key_down[0xA1];
    key_down[0x11] = key_down[0xA2] | key_down[0xA3];
    key_down[0x12] = key_down[0xA4] | key_down[0xA5];
}

/* A raw input record for a window: WM_INPUT with RIM_INPUT and the record as lParam. */
static void raw_deliver(uint64_t hwnd, uint64_t handle) {
    if (win_of(hwnd)) post(hwnd, WM_INPUT, 0, handle);
}

/* Virtual-key code for a raw keycode (level 0 of the core keyboard map). */
static uint8_t raw_vk(unsigned keycode) {
    if (keycode > 255) return 0;
    return vk_for_keysym(keysym_for((uint8_t)keycode, 0));
}

/* Turn pending X events into messages. With block set, wait for at least one event. */
static void pump(int block) {
    xcb_connection_t *c = xbemu_xcb_connection();
    if (!c) return;
    if (!init_atoms(c)) return;
    load_keymap(c);

    /* Raw mouse motion for windows that registered for it. */
    rawinput_poll(raw_deliver, raw_vk);

    int first = 1;
    for (;;) {
        xcb_generic_event_t *ev;
        if (block && first && q_count == 0)
            ev = xcb_wait_for_event(c);
        else
            ev = xcb_poll_for_event(c);
        first = 0;
        if (!ev) break;

        switch (ev->response_type & ~0x80) {
        case XCB_EXPOSE: {
            xcb_expose_event_t *e = (xcb_expose_event_t *)ev;
            Win *w = win_by_xid(e->window);
            if (w && e->count == 0) post((uint64_t)(uintptr_t)w, WM_PAINT, 0, 0);
            break;
        }
        case XCB_CONFIGURE_NOTIFY: {
            xcb_configure_notify_event_t *e = (xcb_configure_notify_event_t *)ev;
            Win *w = win_by_xid(e->window);
            if (w) {
                int resized = (w->w != e->width || w->h != e->height);
                w->x = e->x; w->y = e->y; w->w = e->width; w->h = e->height;
                if (resized)
                    post((uint64_t)(uintptr_t)w, WM_SIZE, 0,
                         (uint64_t)(uint16_t)e->width | ((uint64_t)(uint16_t)e->height << 16));
            }
            break;
        }
        case XCB_KEY_PRESS:
        case XCB_KEY_RELEASE: {
            xcb_key_press_event_t *e = (xcb_key_press_event_t *)ev;
            Win *w = win_by_xid(e->event);
            if (!w) break;
            int down = (ev->response_type & ~0x80) == XCB_KEY_PRESS;
            uint8_t vk = vk_for_keysym(keysym_for(e->detail, (e->state & XCB_MOD_MASK_SHIFT) != 0));
            if (!vk) break;
            int was_down = key_down[vk];
            key_down[vk] = (uint8_t)(down ? 1 : 0);
            update_modifiers();
            update_toggles(e->state);
            /* lParam: repeat count 1, scan code, previous-state bit 30, transition bit 31. */
            uint64_t lp = 1 | ((uint64_t)(e->detail & 0xFF) << 16) |
                          ((uint64_t)(was_down ? 1 : 0) << 30) | ((uint64_t)(down ? 0 : 1) << 31);
            post((uint64_t)(uintptr_t)w, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
            break;
        }
        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE: {
            xcb_button_press_event_t *e = (xcb_button_press_event_t *)ev;
            Win *w = win_by_xid(e->event);
            if (!w) break;
            int down = (ev->response_type & ~0x80) == XCB_BUTTON_PRESS;
            uint64_t mk = 0;
            if (e->state & XCB_BUTTON_MASK_1) mk |= MK_LBUTTON;
            if (e->state & XCB_BUTTON_MASK_2) mk |= MK_MBUTTON;
            if (e->state & XCB_BUTTON_MASK_3) mk |= MK_RBUTTON;
            if (e->state & XCB_MOD_MASK_SHIFT) mk |= MK_SHIFT;
            if (e->state & XCB_MOD_MASK_CONTROL) mk |= MK_CONTROL;
            uint64_t xy = (uint16_t)e->event_x | ((uint64_t)(uint16_t)e->event_y << 16);
            uint64_t hwnd = (uint64_t)(uintptr_t)w;
            if (e->detail == 4 || e->detail == 5) {
                /* Wheel: buttons 4 and 5 are reported as presses only. */
                if (down) {
                    int16_t delta = e->detail == 4 ? WHEEL_DELTA : -WHEEL_DELTA;
                    post(hwnd, WM_MOUSEWHEEL, ((uint64_t)(uint16_t)delta << 16) | mk, xy);
                }
                break;
            }
            uint32_t msg = 0;
            uint8_t vk = 0;
            if (e->detail == 1) { msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; vk = VK_LBUTTON_CODE; }
            else if (e->detail == 2) { msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; vk = VK_MBUTTON_CODE; }
            else if (e->detail == 3) { msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; vk = VK_RBUTTON_CODE; }
            else break;
            key_down[vk] = (uint8_t)(down ? 1 : 0);
            update_modifiers();
            post(hwnd, msg, mk, xy);
            break;
        }
        case XCB_MOTION_NOTIFY: {
            xcb_motion_notify_event_t *e = (xcb_motion_notify_event_t *)ev;
            Win *w = win_by_xid(e->event);
            if (!w) break;
            uint64_t mk = 0;
            if (e->state & XCB_BUTTON_MASK_1) mk |= MK_LBUTTON;
            if (e->state & XCB_BUTTON_MASK_2) mk |= MK_MBUTTON;
            if (e->state & XCB_BUTTON_MASK_3) mk |= MK_RBUTTON;
            uint64_t xy = (uint16_t)e->event_x | ((uint64_t)(uint16_t)e->event_y << 16);
            post((uint64_t)(uintptr_t)w, WM_MOUSEMOVE, mk, xy);
            break;
        }
        case XCB_FOCUS_IN:
        case XCB_FOCUS_OUT: {
            xcb_focus_in_event_t *e = (xcb_focus_in_event_t *)ev;
            Win *w = win_by_xid(e->event);
            if (!w) break;
            int in = (ev->response_type & ~0x80) == XCB_FOCUS_IN;
            if (in) focus_hwnd = (uint64_t)(uintptr_t)w;
            else if (focus_hwnd == (uint64_t)(uintptr_t)w) focus_hwnd = 0;
            post((uint64_t)(uintptr_t)w, in ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
            break;
        }
        case XCB_CLIENT_MESSAGE: {
            xcb_client_message_event_t *e = (xcb_client_message_event_t *)ev;
            Win *w = win_by_xid(e->window);
            if (w && e->type == atom_wm_protocols && e->data.data32[0] == atom_wm_delete)
                post((uint64_t)(uintptr_t)w, WM_CLOSE, 0, 0);
            break;
        }
        default:
            break;
        }
        free(ev);
    }
    xcb_flush(c);
}

/* ---- window classes ---- */

/* WNDCLASSEXW: cbSize 0, style 4, lpfnWndProc 8, lpszClassName 64 (x64 layout). */
static uint64_t u_RegisterClassExW(W64Ctx *c) {
    const uint8_t *wc = (const uint8_t *)(uintptr_t)w64_arg(c, 0);
    if (!wc) { k32_set_last_error(87); return 0; }
    const uint16_t *wname;
    uint64_t wndproc;
    memcpy(&wndproc, wc + 8, 8);
    memcpy(&wname, wc + 64, 8);

    if (nclasses >= MAX_CLASSES) { k32_set_last_error(8); return 0; }
    WndClass *k = &classes[nclasses];
    k32_wide_to_ascii(wname, k->name, sizeof k->name);
    k->wndproc = wndproc;
    k->atom = (uint16_t)(0xC000 + nclasses + 1);
    nclasses++;
    return k->atom;
}

static WndClass *find_class(uint64_t name_or_atom) {
    if (name_or_atom < 0x10000) {
        for (size_t i = 0; i < nclasses; i++)
            if (classes[i].atom == name_or_atom) return &classes[i];
        return NULL;
    }
    const uint16_t *wname = (const uint16_t *)(uintptr_t)name_or_atom;
    char name[128];
    k32_wide_to_ascii(wname, name, sizeof name);
    for (size_t i = 0; i < nclasses; i++)
        if (strcasecmp(classes[i].name, name) == 0) return &classes[i];
    return NULL;
}

/* ---- windows ---- */

/* HWND CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y,
 *                      int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
 * Arguments 4 onward are on the stack. */
static uint64_t u_CreateWindowExW(W64Ctx *c) {
    WndClass *k = find_class(w64_arg(c, 1));
    if (!k) { k32_set_last_error(1407); return 0; } /* ERROR_CANNOT_FIND_WND_CLASS */

    uint32_t style = (uint32_t)w64_arg(c, 3);
    int32_t x = (int32_t)(uint32_t)w64_arg(c, 4);
    int32_t y = (int32_t)(uint32_t)w64_arg(c, 5);
    int32_t w = (int32_t)(uint32_t)w64_arg(c, 6);
    int32_t h = (int32_t)(uint32_t)w64_arg(c, 7);
    if ((uint32_t)x == CW_USEDEFAULT_VAL) x = 0;
    if ((uint32_t)y == CW_USEDEFAULT_VAL) y = 0;
    if ((uint32_t)w == CW_USEDEFAULT_VAL || w <= 0) w = 640;
    if ((uint32_t)h == CW_USEDEFAULT_VAL || h <= 0) h = 480;

    xcb_connection_t *conn = xbemu_xcb_connection();
    xcb_screen_t *scr = xbemu_xcb_screen();
    if (!conn || !scr) { k32_set_last_error(1400); return 0; } /* ERROR_INVALID_WINDOW_HANDLE */

    size_t slot = 0;
    while (slot < MAX_WINDOWS && windows[slot]) slot++;
    if (slot == MAX_WINDOWS) { k32_set_last_error(1450); return 0; }

    Win *win = calloc(1, sizeof *win);
    if (!win) { k32_set_last_error(8); return 0; }
    win->xw = xcb_generate_id(conn);
    win->wndproc = k->wndproc;
    win->style = style;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->alive = 1;

    uint32_t mask = XCB_CW_EVENT_MASK;
    uint32_t values[1] = { XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE |
                           XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
                           XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                           XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_FOCUS_CHANGE };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win->xw, scr->root,
                      (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual, mask, values);

    if (init_atoms(conn)) {
        xcb_atom_t protos[1] = { atom_wm_delete };
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win->xw, atom_wm_protocols,
                            XCB_ATOM_ATOM, 32, 1, protos);
        const uint16_t *wtitle = (const uint16_t *)(uintptr_t)w64_arg(c, 2);
        if (wtitle) {
            char title[256];
            k32_wide_to_ascii(wtitle, title, sizeof title);
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win->xw, atom_wm_name,
                                atom_utf8, 8, (uint32_t)strlen(title), title);
        }
    }

    windows[slot] = win;
    uint64_t hwnd = (uint64_t)(uintptr_t)win;

    if (style & WS_VISIBLE) xcb_map_window(conn, win->xw);
    xcb_flush(conn);

    /* WM_CREATE receives a CREATESTRUCTW; lpCreateParams is its first field. */
    uint64_t cs[10] = { 0 };
    cs[0] = w64_arg(c, 11);
    uint64_t r = send_msg(hwnd, WM_CREATE, 0, (uint64_t)(uintptr_t)cs);
    if ((int64_t)r == -1) {
        xcb_destroy_window(conn, win->xw);
        xcb_flush(conn);
        win->alive = 0;
        windows[slot] = NULL;
        free(win);
        return 0;
    }
    return hwnd;
}

/* Destroy a window: send WM_DESTROY, then tear down the X11 window. */
static int destroy_window(uint64_t hwnd) {
    Win *w = win_of(hwnd);
    if (!w || !w->alive) return 0;
    send_msg(hwnd, WM_DESTROY, 0, 0);
    w->alive = 0;
    xcb_connection_t *conn = xbemu_xcb_connection();
    if (conn) {
        xcb_destroy_window(conn, w->xw);
        xcb_flush(conn);
    }
    return 1;
}

/* BOOL DestroyWindow(HWND) */
static uint64_t u_DestroyWindow(W64Ctx *c) {
    if (!destroy_window(w64_arg(c, 0))) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }
    return 1;
}

/* Default window procedure: WM_CLOSE destroys the window. */
static uint64_t default_proc(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    (void)wp; (void)lp;
    if (msg == WM_CLOSE) destroy_window(hwnd);
    return 0;
}

static uint64_t u_DefWindowProcW(W64Ctx *c) {
    return default_proc(w64_arg(c, 0), (uint32_t)w64_arg(c, 1), w64_arg(c, 2), w64_arg(c, 3));
}

/* BOOL ShowWindow(HWND, int cmd): returns whether the window was visible before. */
static uint64_t u_ShowWindow(W64Ctx *c) {
    Win *w = win_of(w64_arg(c, 0));
    if (!w || !w->alive) return 0;
    int show = (int)(uint32_t)w64_arg(c, 1) != SW_HIDE;
    xcb_connection_t *conn = xbemu_xcb_connection();
    if (conn) {
        if (show) xcb_map_window(conn, w->xw);
        else xcb_unmap_window(conn, w->xw);
        xcb_flush(conn);
    }
    return 0;
}

static uint64_t u_IsWindow(W64Ctx *c) {
    Win *w = win_of(w64_arg(c, 0));
    return w && w->alive;
}

static void write_rect(void *dst, int32_t l, int32_t t, int32_t r, int32_t b) {
    int32_t v[4] = { l, t, r, b };
    memcpy(dst, v, sizeof v);
}

/* BOOL GetClientRect(HWND, LPRECT): the client area is the whole window (no decorations). */
static uint64_t u_GetClientRect(W64Ctx *c) {
    Win *w = win_of(w64_arg(c, 0));
    void *r = (void *)(uintptr_t)w64_arg(c, 1);
    if (!w || !r) return 0;
    write_rect(r, 0, 0, w->w, w->h);
    return 1;
}

static uint64_t u_GetWindowRect(W64Ctx *c) {
    Win *w = win_of(w64_arg(c, 0));
    void *r = (void *)(uintptr_t)w64_arg(c, 1);
    if (!w || !r) return 0;
    write_rect(r, w->x, w->y, w->x + w->w, w->y + w->h);
    return 1;
}

/* BOOL SetWindowPos(HWND, HWND after, int x, int y, int cx, int cy, UINT flags) */
static uint64_t u_SetWindowPos(W64Ctx *c) {
    Win *w = win_of(w64_arg(c, 0));
    if (!w || !w->alive) return 0;
    uint32_t flags = (uint32_t)w64_arg(c, 6);
    if (!(flags & SWP_NOMOVE)) { w->x = (int32_t)(uint32_t)w64_arg(c, 2); w->y = (int32_t)(uint32_t)w64_arg(c, 3); }
    if (!(flags & SWP_NOSIZE)) { w->w = (int32_t)(uint32_t)w64_arg(c, 4); w->h = (int32_t)(uint32_t)w64_arg(c, 5); }
    xcb_connection_t *conn = xbemu_xcb_connection();
    if (conn) {
        uint32_t v[4] = { (uint32_t)w->x, (uint32_t)w->y, (uint32_t)w->w, (uint32_t)w->h };
        xcb_configure_window(conn, w->xw,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
        xcb_flush(conn);
    }
    return 1;
}

/* BOOL MoveWindow(HWND, int x, int y, int w, int h, BOOL repaint) */
static uint64_t u_MoveWindow(W64Ctx *c) {
    Win *w = win_of(w64_arg(c, 0));
    if (!w || !w->alive) return 0;
    w->x = (int32_t)(uint32_t)w64_arg(c, 1);
    w->y = (int32_t)(uint32_t)w64_arg(c, 2);
    w->w = (int32_t)(uint32_t)w64_arg(c, 3);
    w->h = (int32_t)(uint32_t)w64_arg(c, 4);
    xcb_connection_t *conn = xbemu_xcb_connection();
    if (conn) {
        uint32_t v[4] = { (uint32_t)w->x, (uint32_t)w->y, (uint32_t)w->w, (uint32_t)w->h };
        xcb_configure_window(conn, w->xw,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
        xcb_flush(conn);
    }
    return 1;
}

/* LONG_PTR GetWindowLongPtrA/GetWindowLongW(HWND, int index) */
static uint64_t u_GetWindowLong(W64Ctx *c) {
    Win *w = win_of(w64_arg(c, 0));
    int idx = (int)(uint32_t)w64_arg(c, 1);
    if (!w) return 0;
    switch (idx) {
    case GWL_WNDPROC:   return w->wndproc;
    case GWL_STYLE:     return w->style;
    case GWL_EXSTYLE:   return 0;
    case GWLP_USERDATA: return w->userdata;
    default:            return 0;
    }
}

/* LONG SetWindowLongW(HWND, int index, LONG value): returns the previous value. */
static uint64_t u_SetWindowLong(W64Ctx *c) {
    Win *w = win_of(w64_arg(c, 0));
    int idx = (int)(uint32_t)w64_arg(c, 1);
    uint64_t v = w64_arg(c, 2);
    if (!w) return 0;
    uint64_t old = u_GetWindowLong(c);
    switch (idx) {
    case GWL_WNDPROC:   w->wndproc = v; break;
    case GWL_STYLE:     w->style = (uint32_t)v; break;
    case GWLP_USERDATA: w->userdata = v; break;
    default: break;
    }
    return old;
}

/* ---- messages ---- */

/* BOOL PeekMessageW(LPMSG, HWND, UINT min, UINT max, UINT remove) */
static uint64_t u_PeekMessageW(W64Ctx *c) {
    uint8_t *out = (uint8_t *)(uintptr_t)w64_arg(c, 0);
    uint32_t remove = (uint32_t)w64_arg(c, 4);
    pump(0);
    if (q_count == 0) return 0;
    if (out) memcpy(out, &queue[q_head], sizeof(MsgRec));
    if (remove & PM_REMOVE) {
        q_head = (q_head + 1) % MSG_QUEUE;
        q_count--;
    }
    return 1;
}

/* BOOL GetMessageW(LPMSG, HWND, UINT, UINT): 0 when WM_QUIT is received, -1 on error. */
static uint64_t u_GetMessageW(W64Ctx *c) {
    uint8_t *out = (uint8_t *)(uintptr_t)w64_arg(c, 0);
    if (!xbemu_xcb_connection()) return (uint64_t)(int64_t)-1;
    while (q_count == 0) {
        pump(1);
        if (!xbemu_xcb_connection()) return (uint64_t)(int64_t)-1;
    }
    if (out) memcpy(out, &queue[q_head], sizeof(MsgRec));
    uint32_t msg = queue[q_head].message;
    q_head = (q_head + 1) % MSG_QUEUE;
    q_count--;
    return msg != WM_QUIT;
}

/* Character for a virtual-key code, for WM_CHAR. 0 means no character. */
static uint32_t char_for_vk(uint8_t vk) {
    int shift = key_down[0x10];
    if (vk >= 'A' && vk <= 'Z') return (shift ^ key_toggle[0x14]) ? vk : (uint32_t)(vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9') return vk;
    switch (vk) {
    case 0x20: return ' ';
    case 0x0D: return '\r';
    case 0x08: return '\b';
    case 0x09: return '\t';
    case 0x1B: return 27;
    case 0xBD: return shift ? '_' : '-';
    case 0xBB: return shift ? '+' : '=';
    case 0xBC: return shift ? '<' : ',';
    case 0xBE: return shift ? '>' : '.';
    case 0xBF: return shift ? '?' : '/';
    case 0xBA: return shift ? ':' : ';';
    case 0xDE: return shift ? '"' : '\'';
    default:   return 0;
    }
}

/* BOOL TranslateMessage(const MSG *): WM_KEYDOWN produces a WM_CHAR when it has a character. */
static uint64_t u_TranslateMessage(W64Ctx *c) {
    const MsgRec *m = (const MsgRec *)(uintptr_t)w64_arg(c, 0);
    if (!m || (m->message != WM_KEYDOWN)) return 0;
    uint32_t ch = char_for_vk((uint8_t)m->wParam);
    if (!ch) return 0;
    post(m->hwnd, WM_CHAR, ch, m->lParam);
    return 1;
}

/* SHORT GetAsyncKeyState(int vk): high bit set while the key is down. */
static uint64_t u_GetAsyncKeyState(W64Ctx *c) {
    uint32_t vk = (uint32_t)w64_arg(c, 0) & 0xFF;
    return key_down[vk] ? 0x8000 : 0;
}

/* SHORT GetKeyState(int vk): down bit, and the low bit for toggle keys. */
static uint64_t u_GetKeyState(W64Ctx *c) {
    uint32_t vk = (uint32_t)w64_arg(c, 0) & 0xFF;
    uint64_t r = key_down[vk] ? 0x8000 : 0;
    if (vk == 0x14 || vk == 0x90 || vk == 0x91) r |= key_toggle[vk];
    return r;
}

/* HWND GetFocus(void) */
static uint64_t u_GetFocus(W64Ctx *c) {
    (void)c;
    return focus_hwnd;
}

/* HWND SetFocus(HWND) */
static uint64_t u_SetFocus(W64Ctx *c) {
    uint64_t prev = focus_hwnd;
    Win *w = win_of(w64_arg(c, 0));
    if (!w || !w->alive) return 0;
    xcb_connection_t *conn = xbemu_xcb_connection();
    if (conn) {
        xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, w->xw, XCB_CURRENT_TIME);
        xcb_flush(conn);
    }
    focus_hwnd = w64_arg(c, 0);
    return prev;
}

/* BOOL GetCursorPos(LPPOINT): pointer position on the screen. */
static uint64_t u_GetCursorPos(W64Ctx *c) {
    int32_t *pt = (int32_t *)(uintptr_t)w64_arg(c, 0);
    xcb_connection_t *conn = xbemu_xcb_connection();
    xcb_screen_t *scr = xbemu_xcb_screen();
    if (!pt || !conn || !scr) return 0;
    xcb_query_pointer_reply_t *r = xcb_query_pointer_reply(conn, xcb_query_pointer(conn, scr->root), NULL);
    if (!r) return 0;
    pt[0] = r->root_x;
    pt[1] = r->root_y;
    free(r);
    return 1;
}

/* int ShowCursor(BOOL): cursors are not drawn; report the visible count as 0. */
static uint64_t u_ShowCursor(W64Ctx *c) {
    (void)c;
    return 0;
}

/* LRESULT DispatchMessageW(const MSG *) */
static uint64_t u_DispatchMessageW(W64Ctx *c) {
    const MsgRec *m = (const MsgRec *)(uintptr_t)w64_arg(c, 0);
    if (!m) return 0;
    return send_msg(m->hwnd, m->message, m->wParam, m->lParam);
}

/* BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM) */
static uint64_t u_PostMessageW(W64Ctx *c) {
    post(w64_arg(c, 0), (uint32_t)w64_arg(c, 1), w64_arg(c, 2), w64_arg(c, 3));
    return 1;
}

/* LRESULT SendMessageW(HWND, UINT, WPARAM, LPARAM): calls the procedure directly. */
static uint64_t u_SendMessageW(W64Ctx *c) {
    return send_msg(w64_arg(c, 0), (uint32_t)w64_arg(c, 1), w64_arg(c, 2), w64_arg(c, 3));
}

/* VOID PostQuitMessage(int) */
static uint64_t u_PostQuitMessage(W64Ctx *c) {
    post(0, WM_QUIT, (uint32_t)w64_arg(c, 0), 0);
    return 0;
}

/* ---- queries and stubs ---- */

/* HWND GetForegroundWindow(void): the first live window stands in for the foreground one. */
static uint64_t u_GetForegroundWindow(W64Ctx *c) {
    (void)c;
    for (size_t i = 0; i < MAX_WINDOWS; i++)
        if (windows[i] && windows[i]->alive) return (uint64_t)(uintptr_t)windows[i];
    return 0;
}

/* BOOL AdjustWindowRectEx(LPRECT, DWORD style, BOOL menu, DWORD ex): no decorations to add. */
static uint64_t u_AdjustWindowRectEx(W64Ctx *c) {
    return w64_arg(c, 0) != 0;
}

/* int GetSystemMetrics(int): screen size; other metrics are 0. */
static uint64_t u_GetSystemMetrics(W64Ctx *c) {
    int idx = (int)(uint32_t)w64_arg(c, 0);
    xbemu_xcb_connection();
    xcb_screen_t *scr = xbemu_xcb_screen();
    if (!scr) return 0;
    if (idx == SM_CXSCREEN) return scr->width_in_pixels;
    if (idx == SM_CYSCREEN) return scr->height_in_pixels;
    return 0;
}

/* HCURSOR LoadCursorW / HICON LoadIconW: cursors and icons are not drawn; return a placeholder. */
static uint64_t u_LoadCursorW(W64Ctx *c) {
    (void)c;
    return 1;
}

/* BOOL SetRect(LPRECT, int l, int t, int r, int b) */
static uint64_t u_SetRect(W64Ctx *c) {
    void *r = (void *)(uintptr_t)w64_arg(c, 0);
    if (!r) return 0;
    write_rect(r, (int32_t)(uint32_t)w64_arg(c, 1), (int32_t)(uint32_t)w64_arg(c, 2),
               (int32_t)(uint32_t)w64_arg(c, 3), (int32_t)(uint32_t)w64_arg(c, 4));
    return 1;
}

/* BOOL OffsetRect(LPRECT, int dx, int dy) */
static uint64_t u_OffsetRect(W64Ctx *c) {
    int32_t *r = (int32_t *)(uintptr_t)w64_arg(c, 0);
    if (!r) return 0;
    int32_t dx = (int32_t)(uint32_t)w64_arg(c, 1), dy = (int32_t)(uint32_t)w64_arg(c, 2);
    r[0] += dx; r[2] += dx; r[1] += dy; r[3] += dy;
    return 1;
}

/* ---- monitors and display modes ----
 * Monitors come from RandR (display.c). HMONITOR is 0x10001 + index. Mode changes are
 * refused, so the reported modes are the ones the server already has. */

#define HMONITOR_BASE  ((uint64_t)0x10001)
#define ENUM_CURRENT_SETTINGS  ((uint32_t)-1)
#define ENUM_REGISTRY_SETTINGS ((uint32_t)-2)
#define DM_POSITION            0x00000020
#define DM_BITSPERPEL          0x00040000
#define DM_PELSWIDTH           0x00080000
#define DM_PELSHEIGHT          0x00100000
#define DM_DISPLAYFREQUENCY    0x00400000

static int monitor_list(DisplayMonitor *mons, int max) {
    return display_monitors(mons, max);
}

static void write_wide(uint16_t *dst, const char *src, size_t cap) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = (uint8_t)src[i];
    dst[i] = 0;
}

static void device_name(char *out, size_t cap, int index) {
    snprintf(out, cap, "\\\\.\\DISPLAY%d", index + 1);
}

/* BOOL EnumDisplayMonitors(HDC, LPCRECT clip, MONITORENUMPROC proc, LPARAM data) */
static uint64_t u_EnumDisplayMonitors(W64Ctx *c) {
    void *proc = (void *)(uintptr_t)w64_arg(c, 2);
    uint64_t data = w64_arg(c, 3);
    DisplayMonitor mons[DISPLAY_MAX_MONITORS];
    int n = monitor_list(mons, DISPLAY_MAX_MONITORS);
    for (int i = 0; i < n && proc; i++) {
        int32_t rect[4] = { mons[i].x, mons[i].y, mons[i].x + (int32_t)mons[i].width,
                            mons[i].y + (int32_t)mons[i].height };
        /* BOOL CALLBACK MonitorEnumProc(HMONITOR, HDC, LPRECT, LPARAM) */
        uint64_t go = xbemu_call_w64(proc, HMONITOR_BASE + i, 0, (uint64_t)(uintptr_t)rect, data);
        if (!(uint32_t)go) break;
    }
    return 1;
}

/* HMONITOR MonitorFromWindow / MonitorFromPoint: the primary monitor. */
static uint64_t u_MonitorFromPoint(W64Ctx *c) {
    (void)c;
    return HMONITOR_BASE;
}

/* BOOL GetMonitorInfoW(HMONITOR, LPMONITORINFO): MONITORINFO, or MONITORINFOEXW when cbSize says so. */
static uint64_t u_GetMonitorInfoW(W64Ctx *c) {
    uint64_t h = w64_arg(c, 0);
    uint8_t *mi = (uint8_t *)(uintptr_t)w64_arg(c, 1);
    DisplayMonitor mons[DISPLAY_MAX_MONITORS];
    int n = monitor_list(mons, DISPLAY_MAX_MONITORS);
    if (!mi || h < HMONITOR_BASE || h >= HMONITOR_BASE + (uint64_t)n) return 0;
    int i = (int)(h - HMONITOR_BASE);
    uint32_t cb;
    memcpy(&cb, mi, 4);
    int32_t r[4] = { mons[i].x, mons[i].y, mons[i].x + (int32_t)mons[i].width,
                     mons[i].y + (int32_t)mons[i].height };
    uint32_t flags = mons[i].primary ? 1 : 0;   /* MONITORINFOF_PRIMARY */
    memcpy(mi + 4, r, 16);    /* rcMonitor */
    memcpy(mi + 20, r, 16);   /* rcWork: no taskbar is known */
    memcpy(mi + 36, &flags, 4);
    if (cb >= 104) {
        char name[32];
        device_name(name, sizeof name, i);
        write_wide((uint16_t *)(mi + 40), name, 32);
    }
    return 1;
}

/* BOOL EnumDisplaySettingsW(LPCWSTR device, DWORD mode, LPDEVMODEW): mode index in the
 * monitor's list; ENUM_CURRENT_SETTINGS and ENUM_REGISTRY_SETTINGS report the current mode. */
static uint64_t u_EnumDisplaySettingsW(W64Ctx *c) {
    uint32_t mode = (uint32_t)w64_arg(c, 1);
    uint8_t *dm = (uint8_t *)(uintptr_t)w64_arg(c, 2);
    DisplayMonitor mons[DISPLAY_MAX_MONITORS];
    int n = monitor_list(mons, DISPLAY_MAX_MONITORS);
    if (!dm || n == 0) return 0;

    /* The device name, when given, selects the monitor (\\.\DISPLAYn). Default: the first. */
    int mon = 0;
    const uint16_t *dev = (const uint16_t *)(uintptr_t)w64_arg(c, 0);
    if (dev) {
        for (int i = 0; i < n; i++) {
            char name[32];
            device_name(name, sizeof name, i);
            size_t k = 0;
            while (name[k] && dev[k] == (uint8_t)name[k]) k++;
            if (!name[k] && !dev[k]) { mon = i; break; }
        }
    }
    const DisplayMonitor *m = &mons[mon];

    DisplayMode want;
    if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS) {
        want = m->current;
    } else {
        if ((int)mode >= m->nmodes) return 0;
        want = m->modes[mode];
    }

    char name[32];
    device_name(name, sizeof name, mon);
    memset(dm, 0, 220);
    write_wide((uint16_t *)dm, name, 32);   /* dmDeviceName */
    uint16_t spec = 0x0401, size = 220;
    memcpy(dm + 64, &spec, 2);              /* dmSpecVersion */
    memcpy(dm + 68, &size, 2);              /* dmSize */
    uint32_t fields = DM_POSITION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    int32_t px = m->x, py = m->y;
    uint32_t bpp = 32;
    memcpy(dm + 72, &fields, 4);
    memcpy(dm + 76, &px, 4);                /* dmPosition.x */
    memcpy(dm + 80, &py, 4);                /* dmPosition.y */
    memcpy(dm + 168, &bpp, 4);
    memcpy(dm + 172, &want.width, 4);
    memcpy(dm + 176, &want.height, 4);
    memcpy(dm + 184, &want.refresh, 4);
    return 1;
}

/* Mode changes are refused: the screen is not reconfigured. */
static uint64_t u_ChangeDisplaySettingsExW(W64Ctx *c) { (void)c; return DISP_CHANGE_FAILED; }
static uint64_t u_NotSupported(W64Ctx *c) { (void)c; return ERROR_NOT_SUPPORTED; }

static uint64_t u_RegisterRawInputDevices(W64Ctx *c);
static uint64_t u_GetRawInputData(W64Ctx *c);

const HostExport user32_exports[] = {
    { "RegisterClassExW",            u_RegisterClassExW            },
    { "CreateWindowExW",             u_CreateWindowExW             },
    { "DestroyWindow",               u_DestroyWindow               },
    { "DefWindowProcW",              u_DefWindowProcW              },
    { "ShowWindow",                  u_ShowWindow                  },
    { "IsWindow",                    u_IsWindow                    },
    { "GetClientRect",               u_GetClientRect               },
    { "GetWindowRect",               u_GetWindowRect               },
    { "SetWindowPos",                u_SetWindowPos                },
    { "MoveWindow",                  u_MoveWindow                  },
    { "GetWindowLongW",              u_GetWindowLong               },
    { "GetWindowLongPtrA",           u_GetWindowLong               },
    { "SetWindowLongW",              u_SetWindowLong               },
    { "PeekMessageW",                u_PeekMessageW                },
    { "GetMessageW",                 u_GetMessageW                 },
    { "TranslateMessage",            u_TranslateMessage            },
    { "DispatchMessageW",            u_DispatchMessageW            },
    { "PostMessageW",                u_PostMessageW                },
    { "SendMessageW",                u_SendMessageW                },
    { "PostQuitMessage",             u_PostQuitMessage             },
    { "GetForegroundWindow",         u_GetForegroundWindow         },
    { "AdjustWindowRectEx",          u_AdjustWindowRectEx          },
    { "GetSystemMetrics",            u_GetSystemMetrics            },
    { "LoadCursorW",                 u_LoadCursorW                 },
    { "LoadIconW",                   u_LoadCursorW                 },
    { "SetRect",                     u_SetRect                     },
    { "OffsetRect",                  u_OffsetRect                  },
    { "ChangeDisplaySettingsExW",    u_ChangeDisplaySettingsExW    },
    { "EnumDisplaySettingsW",        u_EnumDisplaySettingsW        },
    { "GetMonitorInfoW",             u_GetMonitorInfoW             },
    { "MonitorFromPoint",            u_MonitorFromPoint            },
    { "MonitorFromWindow",           u_MonitorFromPoint            },
    { "QueryDisplayConfig",          u_NotSupported                },
    { "GetDisplayConfigBufferSizes", u_NotSupported                },
    { "DisplayConfigGetDeviceInfo",  u_NotSupported                },
    { "EnumDisplayMonitors",         u_EnumDisplayMonitors         },
    { "GetAsyncKeyState",            u_GetAsyncKeyState            },
    { "RegisterRawInputDevices",     u_RegisterRawInputDevices     },
    { "GetRawInputData",             u_GetRawInputData             },
    { "GetKeyState",                 u_GetKeyState                 },
    { "GetFocus",                    u_GetFocus                    },
    { "SetFocus",                    u_SetFocus                    },
    { "GetCursorPos",                u_GetCursorPos                },
    { "ShowCursor",                  u_ShowCursor                  },
    { NULL, NULL },
};

/* ---- raw input ---- */

/* RAWINPUTDEVICE (16 bytes): usUsagePage 0, usUsage 2, dwFlags 4, hwndTarget 8.
 * Generic desktop page (1): usage 2 is the mouse, 6 the keyboard. */
#define RIDEV_REMOVE 0x00000001u

/* BOOL RegisterRawInputDevices(PCRAWINPUTDEVICE, UINT count, UINT size) */
static uint64_t u_RegisterRawInputDevices(W64Ctx *c) {
    const uint8_t *devs = (const uint8_t *)(uintptr_t)w64_arg(c, 0);
    uint32_t n = (uint32_t)w64_arg(c, 1);
    uint32_t size = (uint32_t)w64_arg(c, 2);
    if (!devs || size != 16) { k32_set_last_error(87); return 0; }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *d = devs + i * 16;
        uint16_t page, usage;
        uint32_t flags;
        uint64_t hwnd;
        memcpy(&page, d + 0, 2);
        memcpy(&usage, d + 2, 2);
        memcpy(&flags, d + 4, 4);
        memcpy(&hwnd, d + 8, 8);
        int remove = (flags & RIDEV_REMOVE) != 0;
        if (page == 1 && usage == 2) rawinput_set_target(hwnd, RAWINPUT_KIND_MOUSE, remove);
        if (page == 1 && usage == 6) rawinput_set_target(hwnd, RAWINPUT_KIND_KEYBOARD, remove);
    }
    return 1;
}

/* UINT GetRawInputData(HRAWINPUT, UINT command, LPVOID data, PUINT size, UINT header size) */
static uint64_t u_GetRawInputData(W64Ctx *c) {
    uint32_t *size = (uint32_t *)(uintptr_t)w64_arg(c, 3);
    if (!size) return (uint32_t)-1;
    return rawinput_get_data(w64_arg(c, 0), (uint32_t)w64_arg(c, 1), (void *)(uintptr_t)w64_arg(c, 2),
                             size, (uint32_t)w64_arg(c, 4));
}

/* ---- support for the Vulkan shim ---- */

int xbemu_hwnd_to_xcb(uint64_t hwnd, xcb_window_t *out) {
    Win *w = win_of(hwnd);
    if (!w || !w->alive) return 0;
    *out = w->xw;
    return 1;
}

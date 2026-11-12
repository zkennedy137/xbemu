/*
 * Monitor and display-mode data from the X server, through RandR (Xlib). This is a
 * separate Display connection, because Xlib and xcb should not share one connection.
 * Mode changes are not implemented: the server's configuration is read, not changed.
 */
#define _DEFAULT_SOURCE
#include "display.h"

#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

static Display *dpy;
static int dpy_tried;

static Display *display_connection(void) {
    if (!dpy_tried) {
        dpy_tried = 1;
        dpy = XOpenDisplay(NULL);
    }
    return dpy;
}

static uint32_t refresh_of(const XRRModeInfo *m) {
    if (!m || m->hTotal == 0 || m->vTotal == 0 || m->dotClock == 0) return 60;
    double hz = (double)m->dotClock / ((double)m->hTotal * (double)m->vTotal);
    uint32_t r = (uint32_t)(hz + 0.5);
    return r ? r : 60;
}

static const XRRModeInfo *find_mode(const XRRScreenResources *res, RRMode id) {
    for (int i = 0; i < res->nmode; i++)
        if (res->modes[i].id == id) return &res->modes[i];
    return NULL;
}

static int fill_whole_screen(DisplayMonitor *out) {
    Display *d = display_connection();
    if (!d) return 0;
    int scr = DefaultScreen(d);
    memset(out, 0, sizeof *out);
    strcpy(out->name, "screen");
    out->width = (uint32_t)DisplayWidth(d, scr);
    out->height = (uint32_t)DisplayHeight(d, scr);
    out->primary = 1;
    out->current.width = out->width;
    out->current.height = out->height;
    out->current.refresh = 60;
    out->modes[0] = out->current;
    out->nmodes = 1;
    return 1;
}

int display_monitors(DisplayMonitor *out, int max) {
    Display *d = display_connection();
    if (!d || max <= 0) return 0;
    Window root = RootWindow(d, DefaultScreen(d));
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(d, root);
    int n = 0;
    if (res) {
        for (int i = 0; i < res->noutput && n < max; i++) {
            XRROutputInfo *oi = XRRGetOutputInfo(d, res, res->outputs[i]);
            if (!oi) continue;
            if (oi->connection == RR_Connected && oi->crtc != None) {
                XRRCrtcInfo *ci = XRRGetCrtcInfo(d, res, oi->crtc);
                if (ci) {
                    DisplayMonitor *m = &out[n];
                    memset(m, 0, sizeof *m);
                    snprintf(m->name, sizeof m->name, "%s", oi->name);
                    m->x = ci->x;
                    m->y = ci->y;
                    m->width = ci->width;
                    m->height = ci->height;
                    m->current.width = ci->width;
                    m->current.height = ci->height;
                    m->current.refresh = 60;
                    const XRRModeInfo *cur = find_mode(res, ci->mode);
                    if (cur) m->current.refresh = refresh_of(cur);
                    m->primary = (n == 0);
                    for (int k = 0; k < oi->nmode && m->nmodes < DISPLAY_MAX_MODES; k++) {
                        const XRRModeInfo *mi = find_mode(res, oi->modes[k]);
                        if (!mi) continue;
                        m->modes[m->nmodes].width = mi->width;
                        m->modes[m->nmodes].height = mi->height;
                        m->modes[m->nmodes].refresh = refresh_of(mi);
                        m->nmodes++;
                    }
                    if (m->nmodes == 0) m->modes[m->nmodes++] = m->current;
                    n++;
                    XRRFreeCrtcInfo(ci);
                }
            }
            XRRFreeOutputInfo(oi);
        }
        XRRFreeScreenResources(res);
    }
    if (n == 0 && fill_whole_screen(&out[0])) n = 1;
    return n;
}

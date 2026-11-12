#ifndef XBEMU_DISPLAY_H
#define XBEMU_DISPLAY_H

#include <stdint.h>

#define DISPLAY_MAX_MONITORS 8
#define DISPLAY_MAX_MODES    128

typedef struct DisplayMode {
    uint32_t width, height, refresh;   /* refresh in Hz (60 when the server does not say) */
} DisplayMode;

typedef struct DisplayMonitor {
    char name[32];                     /* output name, for example "HDMI-1" or "screen" */
    int32_t x, y;                      /* position on the virtual desktop */
    uint32_t width, height;
    int primary;
    int nmodes;
    DisplayMode modes[DISPLAY_MAX_MODES];
    DisplayMode current;
} DisplayMonitor;

/*
 * Connected monitors, from RandR. Falls back to the whole X screen when RandR reports
 * nothing. Returns the count (at least 1 when the X server is reachable, else 0).
 */
int display_monitors(DisplayMonitor *out, int max);

#endif

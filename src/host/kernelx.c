/*
 * Shims for the Xbox ERA kernel-side DLL (kernelx.dll).
 *
 * Only the functions the test program uses are implemented. Behavior follows
 * the Win32 reimplementations in xwine1/SlimEra, except where noted below.
 */
#define _DEFAULT_SOURCE
#include "hostlib.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* CONSOLE_TYPE values from SlimEra/era.h. */
#define CONSOLE_TYPE_XBOX_ONE 1

/*
 * Placeholder values. SlimEra reads these from the host Windows version;
 * an emulator must report a fixed guest OS version instead. 10.0.15063 matches
 * the example version in xwine1/XDLCompiler's README.
 */
#define GUEST_OS_MAJOR   10
#define GUEST_OS_MINOR   0
#define GUEST_OS_BUILD   15063
#define GUEST_OS_REV     0

/* CONSOLE_TYPE GetConsoleType(void) */
static uint64_t kx_GetConsoleType(W64Ctx *c) {
    (void)c;
    return CONSOLE_TYPE_XBOX_ONE;
}

/* VOID GetSystemOSVersion(LPSYSTEMOSVERSIONINFO) */
static uint64_t kx_GetSystemOSVersion(W64Ctx *c) {
    struct {
        uint8_t major, minor;
        uint16_t build, revision;
    } *v = (void *)(uintptr_t)w64_arg(c, 0);
    if (v) {
        v->major = GUEST_OS_MAJOR;
        v->minor = GUEST_OS_MINOR;
        v->build = GUEST_OS_BUILD;
        v->revision = GUEST_OS_REV;
    }
    return 0;
}

/* PVOID XMemAlloc(SIZE_T size, ULONGLONG attributes) */
static uint64_t kx_XMemAlloc(W64Ctx *c) {
    size_t size = (size_t)w64_arg(c, 0);
    if (size == 0) return 0;
    return (uint64_t)(uintptr_t)malloc(size);
}

/* void XMemFree(PVOID address, ULONGLONG attributes) */
static uint64_t kx_XMemFree(W64Ctx *c) {
    free((void *)(uintptr_t)w64_arg(c, 0));
    return 0;
}

const HostExport kernelx_exports[] = {
    { "GetConsoleType",     kx_GetConsoleType     },
    { "GetSystemOSVersion", kx_GetSystemOSVersion },
    { "XMemAlloc",          kx_XMemAlloc          },
    { "XMemFree",           kx_XMemFree           },
    { NULL, NULL },
};

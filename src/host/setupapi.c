/*
 * setupapi.dll shims: device-interface enumeration. There is no Windows device
 * database, so enumeration finds nothing and callers take their fallback path.
 */
#define _DEFAULT_SOURCE
#include "kernel32_impl.h"

#include <stdint.h>

#define ERROR_NO_MORE_ITEMS 259u
#define INVALID_HANDLE ((uint64_t)-1)

/* HDEVINFO SetupDiGetClassDevsW(...): no device information sets. */
static uint64_t sd_GetClassDevsW(W64Ctx *c) {
    (void)c;
    k32_set_last_error(WIN_ERROR_NOT_SUPPORTED);
    return INVALID_HANDLE;
}

/* BOOL SetupDiEnumDeviceInterfaces(...): nothing to enumerate. */
static uint64_t sd_EnumDeviceInterfaces(W64Ctx *c) {
    (void)c;
    k32_set_last_error(ERROR_NO_MORE_ITEMS);
    return 0;
}

/* BOOL SetupDiGetDeviceInterfaceDetailW(...) */
static uint64_t sd_GetDeviceInterfaceDetailW(W64Ctx *c) {
    (void)c;
    k32_set_last_error(WIN_ERROR_NOT_SUPPORTED);
    return 0;
}

/* HKEY SetupDiOpenDevRegKey(...): no device registry keys. */
static uint64_t sd_OpenDevRegKey(W64Ctx *c) {
    (void)c;
    k32_set_last_error(WIN_ERROR_NOT_SUPPORTED);
    return INVALID_HANDLE;
}

const HostExport setupapi_exports[] = {
    { "SetupDiGetClassDevsW",               sd_GetClassDevsW             },
    { "SetupDiEnumDeviceInterfaces",        sd_EnumDeviceInterfaces      },
    { "SetupDiGetDeviceInterfaceDetailW",   sd_GetDeviceInterfaceDetailW },
    { "SetupDiOpenDevRegKey",               sd_OpenDevRegKey             },
    { NULL, NULL },
};

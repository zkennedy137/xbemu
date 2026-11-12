/*
 * kernel32 module loading: LoadLibrary, GetModuleHandle, GetProcAddress.
 */
#define _DEFAULT_SOURCE
#include "kernel32_impl.h"
#include "../pe.h"
#include "paths.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void lower_name(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

/* Module handle for a name: a PE module's base, or a host DLL handle. NULL if neither. */
static void *handle_for_name(const char *name) {
    char lower[256];
    lower_name(lower, sizeof lower, name);
    PeModule *m = pe_find_module(lower);
    if (m) return m->base;
    if (hostlib_has_dll(lower)) return (void *)hostlib_module_handle(lower);
    return NULL;
}

/* HMODULE LoadLibraryA(LPCSTR name) */
static uint64_t k32_LoadLibraryA(W64Ctx *c) {
    const char *name = (const char *)(uintptr_t)w64_arg(c, 0);
    char err[256];
    if (!name) { k32_set_last_error(WIN_ERROR_MOD_NOT_FOUND); return 0; }

    PeModule *m = pe_load_module(name, err, sizeof err);
    if (m) return (uint64_t)(uintptr_t)m->base;

    void *h = handle_for_name(name);
    if (h) return (uint64_t)(uintptr_t)h;

    fprintf(stderr, "[xbemu] LoadLibrary(%s) failed: %s\n", name, err);
    k32_set_last_error(WIN_ERROR_MOD_NOT_FOUND);
    return 0;
}

/* HMODULE LoadLibraryW(LPCWSTR name) */
static uint64_t k32_LoadLibraryW(W64Ctx *c) {
    char name[256];
    k32_wide_to_ascii((const uint16_t *)(uintptr_t)w64_arg(c, 0), name, sizeof name);
    W64Ctx a = *c;
    a.r[0] = (uint64_t)(uintptr_t)name;
    return k32_LoadLibraryA(&a);
}

/* BOOL FreeLibrary(HMODULE): modules stay loaded for the life of the process. */
static uint64_t k32_FreeLibrary(W64Ctx *c) {
    (void)c;
    return 1;
}

/* HMODULE GetModuleHandleA(LPCSTR name): NULL means the program itself. */
static uint64_t k32_GetModuleHandleA(W64Ctx *c) {
    const char *name = (const char *)(uintptr_t)w64_arg(c, 0);
    if (!name) {
        PeModule *m = pe_main_module();
        return m ? (uint64_t)(uintptr_t)m->base : 0;
    }
    void *h = handle_for_name(name);
    if (!h) k32_set_last_error(WIN_ERROR_MOD_NOT_FOUND);
    return (uint64_t)(uintptr_t)h;
}

/* HMODULE GetModuleHandleW(LPCWSTR name) */
static uint64_t k32_GetModuleHandleW(W64Ctx *c) {
    const uint16_t *wname = (const uint16_t *)(uintptr_t)w64_arg(c, 0);
    if (!wname) return k32_GetModuleHandleA(c);
    char name[256];
    k32_wide_to_ascii(wname, name, sizeof name);
    W64Ctx a = *c;
    a.r[0] = (uint64_t)(uintptr_t)name;
    return k32_GetModuleHandleA(&a);
}

#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x4u

/* BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out)
 * With FROM_ADDRESS, name is an address inside the module. */
static uint64_t k32_GetModuleHandleExA(W64Ctx *c) {
    uint32_t flags = (uint32_t)w64_arg(c, 0);
    uint64_t name = w64_arg(c, 1);
    void **out = (void **)(uintptr_t)w64_arg(c, 2);

    void *h = NULL;
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        PeModule *m = pe_find_module_by_addr((const void *)(uintptr_t)name);
        h = m ? m->base : NULL;
    } else {
        W64Ctx a = *c;
        a.r[0] = name;
        h = (void *)(uintptr_t)k32_GetModuleHandleA(&a);
    }
    if (out) *out = h;
    if (!h) { k32_set_last_error(WIN_ERROR_MOD_NOT_FOUND); return 0; }
    return 1;
}

/* ---- GetProcAddress ----
 * Export of a PE module: returns the guest function address directly.
 * Export of a host DLL: returns a stub that calls the host shim. Stubs are cached,
 * because the stub region is small and DXVK looks up the same names repeatedly. */

#define PROC_CACHE_SIZE 512

typedef struct ProcCacheEntry {
    const void *handle;
    char name[96];
    void *stub;
} ProcCacheEntry;

static ProcCacheEntry proc_cache[PROC_CACHE_SIZE];
static size_t proc_cache_count;

static void *host_proc(const void *handle, const char *name) {
    const char *dll = hostlib_dll_for_handle(handle);
    if (!dll) return NULL;
    for (size_t i = 0; i < proc_cache_count; i++)
        if (proc_cache[i].handle == handle && strcmp(proc_cache[i].name, name) == 0)
            return proc_cache[i].stub;

    void *stub = hostlib_resolve(dll, name);
    if (!stub || proc_cache_count >= PROC_CACHE_SIZE) return stub;

    ProcCacheEntry *e = &proc_cache[proc_cache_count++];
    e->handle = handle;
    snprintf(e->name, sizeof e->name, "%s", name);
    e->stub = stub;
    return stub;
}

/* FARPROC GetProcAddress(HMODULE, LPCSTR name) */
static uint64_t k32_GetProcAddress(W64Ctx *c) {
    const void *h = (const void *)(uintptr_t)w64_arg(c, 0);
    const char *name = (const char *)(uintptr_t)w64_arg(c, 1);

    if (!h || !name || (uintptr_t)name < 0x10000) {
        /* Ordinal lookups are not supported yet. */
        k32_set_last_error(WIN_ERROR_PROC_NOT_FOUND);
        return 0;
    }

    void *addr = NULL;
    PeModule *m = pe_find_module_by_addr(h);
    if (m && m->base == h) addr = pe_proc_address(m, name);
    else addr = host_proc(h, name);

    if (!addr) k32_set_last_error(WIN_ERROR_PROC_NOT_FOUND);
    return (uint64_t)(uintptr_t)addr;
}

/* DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD size) */
static uint64_t k32_GetModuleFileNameA(W64Ctx *c) {
    const uint8_t *h = (const uint8_t *)(uintptr_t)w64_arg(c, 0);
    char *buf = (char *)(uintptr_t)w64_arg(c, 1);
    uint32_t size = (uint32_t)w64_arg(c, 2);

    PeModule *m = h ? pe_find_module_by_addr(h) : pe_main_module();
    if (!m || !m->path || !buf || !size) { k32_set_last_error(WIN_ERROR_MOD_NOT_FOUND); return 0; }
    char *guest = host_to_guest_path(m->path);
    if (!guest) { k32_set_last_error(8); return 0; }
    size_t n = strlen(guest);
    if (n >= size) n = size - 1;
    memcpy(buf, guest, n);
    buf[n] = 0;
    free(guest);
    return (uint32_t)n;
}

/* DWORD GetModuleFileNameW(HMODULE, LPWSTR buf, DWORD size): ASCII paths only. */
static uint64_t k32_GetModuleFileNameW(W64Ctx *c) {
    const uint8_t *h = (const uint8_t *)(uintptr_t)w64_arg(c, 0);
    uint16_t *buf = (uint16_t *)(uintptr_t)w64_arg(c, 1);
    uint32_t size = (uint32_t)w64_arg(c, 2);

    PeModule *m = h ? pe_find_module_by_addr(h) : pe_main_module();
    if (!m || !m->path || !buf || !size) { k32_set_last_error(WIN_ERROR_MOD_NOT_FOUND); return 0; }
    char *guest = host_to_guest_path(m->path);
    if (!guest) { k32_set_last_error(8); return 0; }
    size_t n = strlen(guest);
    if (n >= size) n = size - 1;
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)guest[i];
    buf[n] = 0;
    free(guest);
    return (uint32_t)n;
}

const HostExport kernel32_loader_exports[] = {
    { "GetModuleFileNameA",   k32_GetModuleFileNameA   },
    { "GetModuleFileNameW",   k32_GetModuleFileNameW   },
    { "LoadLibraryA",         k32_LoadLibraryA         },
    { "LoadLibraryW",         k32_LoadLibraryW         },
    { "FreeLibrary",          k32_FreeLibrary          },
    { "GetModuleHandleA",     k32_GetModuleHandleA     },
    { "GetModuleHandleW",     k32_GetModuleHandleW     },
    { "GetModuleHandleExA",   k32_GetModuleHandleExA   },
    { "GetProcAddress",       k32_GetProcAddress       },
    { NULL, NULL },
};

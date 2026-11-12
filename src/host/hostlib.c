#define _DEFAULT_SOURCE
#include "hostlib.h"

#include <string.h>
#include <strings.h>

/* Dynamic resolvers return a guest-callable stub for names not in the static tables. */
typedef void *(*DynamicResolve)(const char *dll, const char *name);

/* Implemented in vulkan.c. */
void *vulkan_dynamic_resolve(const char *dll, const char *name);

typedef struct HostDll {
    const char *dll;
    const HostExport *const *tables; /* NULL-terminated list of export tables */
    const HostData *data;            /* NULL-terminated data exports, or NULL */
    DynamicResolve dynamic;          /* may be NULL */
    char handle_marker;              /* address used as the module handle */
} HostDll;

static const HostExport *const kernel32_tables[] = {
    kernel32_exports, kernel32_sync_exports, kernel32_loader_exports, kernel32_seh_exports, kernel32_file_exports, NULL,
};
static const HostExport *const kernelx_tables[]  = { kernelx_exports, NULL };
static const HostExport *const msvcrt_tables[]   = { msvcrt_exports, NULL };
static const HostExport *const advapi32_tables[] = { advapi32_exports, NULL };
static const HostExport *const user32_tables[]   = { user32_exports, NULL };
static const HostExport *const xaudio2_tables[]  = { xaudio2_exports, NULL };
static const HostExport *const gdi32_tables[]    = { gdi32_exports, NULL };
static const HostExport *const setupapi_tables[] = { setupapi_exports, NULL };
static const HostExport *const imm32_tables[]    = { imm32_exports, NULL };
static const HostExport *const vulkan_tables[]   = { vulkan_exports, NULL };

static HostDll host_dlls[] = {
    { "kernel32.dll",   kernel32_tables, NULL,        NULL,                    0 },
    { "kernelx.dll",    kernelx_tables,  NULL,        NULL,                    0 },
    { "msvcrt.dll",     msvcrt_tables,   msvcrt_data, NULL,                    0 },
    { "advapi32.dll",   advapi32_tables, NULL,        NULL,                    0 },
    { "user32.dll",     user32_tables,   NULL,        NULL,                    0 },
    { "xaudio2_9.dll",  xaudio2_tables,  NULL,        NULL,                    0 },
    { "gdi32.dll",      gdi32_tables,    NULL,        NULL,                    0 },
    { "setupapi.dll",   setupapi_tables, NULL,        NULL,                    0 },
    { "imm32.dll",      imm32_tables,    NULL,        NULL,                    0 },
    { "xaudio2_8.dll",  xaudio2_tables,  NULL,        NULL,                    0 },
    { "vulkan-1.dll",   vulkan_tables,   NULL,        vulkan_dynamic_resolve,  0 },
    /* Wine's Vulkan loader name. DXVK tries it first. */
    { "winevulkan.dll", vulkan_tables,   NULL,        vulkan_dynamic_resolve,  0 },
};

static const HostDll *find_dll(const char *dll) {
    for (size_t i = 0; i < sizeof host_dlls / sizeof host_dlls[0]; i++)
        if (strcasecmp(host_dlls[i].dll, dll) == 0)
            return &host_dlls[i];
    return NULL;
}

int hostlib_has_dll(const char *dll) {
    return find_dll(dll) != NULL;
}

void *hostlib_data(const char *dll, const char *name) {
    const HostDll *d = find_dll(dll);
    if (!d || !d->data) return NULL;
    for (const HostData *e = d->data; e->name; e++)
        if (strcmp(e->name, name) == 0) return e->addr;
    return NULL;
}

static W64Fn static_find(const HostDll *d, const char *name) {
    for (const HostExport *const *t = d->tables; *t; t++)
        for (const HostExport *e = *t; e->name; e++)
            if (strcmp(e->name, name) == 0)
                return e->fn;
    return NULL;
}

void *hostlib_resolve(const char *dll, const char *name) {
    const HostDll *d = find_dll(dll);
    if (!d) return NULL;
    W64Fn fn = static_find(d, name);
    if (fn) return w64_make_stub(d->dll, name, fn);
    if (d->dynamic) return d->dynamic(d->dll, name);
    return NULL;
}

const void *hostlib_module_handle(const char *dll) {
    const HostDll *d = find_dll(dll);
    return d ? &d->handle_marker : NULL;
}

const char *hostlib_dll_for_handle(const void *handle) {
    for (size_t i = 0; i < sizeof host_dlls / sizeof host_dlls[0]; i++)
        if (&host_dlls[i].handle_marker == handle) return host_dlls[i].dll;
    return NULL;
}

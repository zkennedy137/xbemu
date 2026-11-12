#ifndef XBEMU_HOSTLIB_H
#define XBEMU_HOSTLIB_H

#include "../w64abi.h"

typedef struct HostExport {
    const char *name; /* NULL terminates an export table */
    W64Fn fn;
} HostExport;

/* Export tables. kernel32 is split across several files. */
extern const HostExport kernel32_exports[];
extern const HostExport kernel32_sync_exports[];
extern const HostExport kernel32_loader_exports[];
extern const HostExport kernel32_seh_exports[];
extern const HostExport kernel32_file_exports[];
extern const HostExport kernelx_exports[];
extern const HostExport msvcrt_exports[];
extern const HostExport vulkan_exports[];
extern const HostExport advapi32_exports[];
extern const HostExport xaudio2_exports[];
extern const HostExport gdi32_exports[];
extern const HostExport setupapi_exports[];
extern const HostExport imm32_exports[];
extern const HostExport user32_exports[];

/* Data exports (variables), such as msvcrt's _fmode. NULL-terminated per table. */
typedef struct HostData {
    const char *name;
    void *addr;
} HostData;
extern HostData msvcrt_data[];

/* Address of a host-provided data export, or NULL. The IAT gets this address directly. */
void *hostlib_data(const char *dll, const char *name);

/* Command-line arguments for the guest C runtime (__getmainargs). */
void hostlib_set_args(int argc, char **argv);

/* Is this DLL provided by host shims (as opposed to a PE file or a stub)? */
int hostlib_has_dll(const char *dll);

/*
 * Guest-callable address for dll!name, or NULL if the host has no such export.
 * Checks the static export tables first, then the DLL's dynamic resolver (if any).
 * Each call creates a stub, so callers that repeat lookups should cache the result.
 */
void *hostlib_resolve(const char *dll, const char *name);

/* Handle used for a host DLL in GetModuleHandle/GetProcAddress. NULL if not a host DLL. */
const void *hostlib_module_handle(const char *dll);
/* Reverse lookup from a handle returned by hostlib_module_handle. */
const char *hostlib_dll_for_handle(const void *handle);

#endif

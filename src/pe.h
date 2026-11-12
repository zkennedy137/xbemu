#ifndef XBEMU_PE_H
#define XBEMU_PE_H

#include <stddef.h>
#include <stdint.h>

/* A PE image mapped into this process. Guest addresses are host addresses. */
typedef struct PeModule {
    char name[128];     /* lowercase file basename, e.g. "d3d11.dll" */
    char *path;         /* path the file was loaded from */
    uint8_t *base;      /* HMODULE */
    size_t size;        /* SizeOfImage */
    uint8_t *entry;     /* AddressOfEntryPoint */
    int is_dll;         /* IMAGE_FILE_DLL: entry is DllMain */
    uint32_t exc_rva;   /* exception directory: RUNTIME_FUNCTION table (x64 unwind info) */
    uint32_t exc_size;
    struct PeModule *next;
} PeModule;

/* Directories searched for DLLs that are imported by name. */
void pe_add_search_dir(const char *dir);

/*
 * Load a PE image by path, or by file name searched in the search directories.
 * Maps sections, applies relocations, binds imports (to other PE modules, to
 * host shims, or to logging stubs), sets section protections, and calls DllMain
 * for DLLs. Returns an already-loaded module if the name matches. NULL on error,
 * with a message in err.
 */
PeModule *pe_load_module(const char *path_or_name, char *err, size_t errlen);

PeModule *pe_find_module(const char *basename_or_path);
PeModule *pe_find_module_by_addr(const void *addr);
PeModule *pe_main_module(void);

/*
 * RUNTIME_FUNCTION covering pc in module m (12 bytes: begin, end, unwind-info RVAs),
 * or NULL if pc is not in a function with unwind data (leaf or outside the module).
 */
const uint8_t *pe_runtime_function(PeModule *m, uint64_t pc);

/* Look up an export by name. NULL if missing or forwarded (forwarders not supported). */
void *pe_proc_address(PeModule *m, const char *name);

#endif

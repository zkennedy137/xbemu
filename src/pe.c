#define _DEFAULT_SOURCE
#include "pe.h"
#include "w64abi.h"
#include "host/hostlib.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PE_MACHINE_AMD64        0x8664
#define PE_FILE_DLL             0x2000
#define PE_OPT_MAGIC_PE32PLUS   0x20b
#define PE_SCN_MEM_EXECUTE      0x20000000u
#define PE_SCN_MEM_WRITE        0x80000000u
#define PE_DIR_EXPORT           0
#define PE_DIR_IMPORT           1
#define PE_DIR_BASERELOC        5
#define PE_REL_ABSOLUTE         0
#define PE_REL_HIGHLOW          3
#define PE_REL_DIR64            10
#define PE_ORDINAL_FLAG64       (1ULL << 63)
#define PAGE_SIZE_BYTES         4096u
#define DLL_PROCESS_ATTACH      1

static PeModule *modules;
static PeModule *main_module;   /* first module loaded: the program */
static char *search_dirs[16];
static int nsearch_dirs;

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static int img_ok(const PeModule *m, uint64_t rva, uint64_t n) {
    return rva <= m->size && n <= m->size - rva;
}
static int file_ok(size_t fsize, uint64_t off, uint64_t n) {
    return off <= fsize && n <= fsize - off;
}
static int sec_prot(uint32_t ch) {
    int p = PROT_READ;
    if (ch & PE_SCN_MEM_EXECUTE) p |= PROT_EXEC;
    if (ch & PE_SCN_MEM_WRITE)   p |= PROT_WRITE;
    return p;
}

static PeModule *load_from_file(const char *path, char *err, size_t errlen);

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void lower_copy(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

void pe_add_search_dir(const char *dir) {
    if (nsearch_dirs < (int)(sizeof search_dirs / sizeof search_dirs[0]))
        search_dirs[nsearch_dirs++] = strdup(dir);
}

PeModule *pe_find_module(const char *name) {
    char want[128];
    lower_copy(want, sizeof want, basename_of(name));
    for (PeModule *m = modules; m; m = m->next)
        if (strcmp(m->name, want) == 0) return m;
    return NULL;
}

PeModule *pe_find_module_by_addr(const void *addr) {
    const uint8_t *a = addr;
    for (PeModule *m = modules; m; m = m->next)
        if (a >= m->base && a < m->base + m->size) return m;
    return NULL;
}

PeModule *pe_main_module(void) {
    return main_module;
}

const uint8_t *pe_runtime_function(PeModule *m, uint64_t pc) {
    if (!m || !m->exc_rva || pc < (uint64_t)(uintptr_t)m->base) return NULL;
    uint64_t rva = pc - (uint64_t)(uintptr_t)m->base;
    if (rva >= m->size || !img_ok(m, m->exc_rva, m->exc_size)) return NULL;

    /* The table is sorted by begin address: binary search. */
    size_t lo = 0, hi = m->exc_size / 12;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const uint8_t *rf = m->base + m->exc_rva + mid * 12;
        uint32_t begin = rd32(rf), end = rd32(rf + 4);
        if (rva < begin) hi = mid;
        else if (rva >= end) lo = mid + 1;
        else return rf;
    }
    return NULL;
}

void *pe_proc_address(PeModule *m, const char *name) {
    if (!m || !m->base) return NULL;

    /* Export directory is data directory 0. Read it from the mapped headers. */
    uint32_t nt = rd32(m->base + 0x3C);
    const uint8_t *opt = m->base + nt + 24;
    uint32_t erva = rd32(opt + 112 + PE_DIR_EXPORT * 8);
    uint32_t esz = rd32(opt + 112 + PE_DIR_EXPORT * 8 + 4);
    if (!erva || !img_ok(m, erva, 40)) return NULL;

    const uint8_t *ed = m->base + erva;
    uint32_t nfuncs = rd32(ed + 20), nnames = rd32(ed + 24);
    uint32_t funcs = rd32(ed + 28), names = rd32(ed + 32), ords = rd32(ed + 36);
    if (!img_ok(m, funcs, (uint64_t)nfuncs * 4) ||
        !img_ok(m, names, (uint64_t)nnames * 4) ||
        !img_ok(m, ords, (uint64_t)nnames * 2))
        return NULL;

    for (uint32_t i = 0; i < nnames; i++) {
        uint32_t name_rva = rd32(m->base + names + i * 4);
        if (!img_ok(m, name_rva, 1)) continue;
        if (strcmp((const char *)(m->base + name_rva), name) != 0) continue;

        uint16_t ord = rd16(m->base + ords + i * 2);
        if (ord >= nfuncs) return NULL;
        uint32_t frva = rd32(m->base + funcs + ord * 4);
        if (frva >= erva && frva < erva + esz) {
            fprintf(stderr, "[xbemu] forwarded export %s!%s not supported\n", m->name, name);
            return NULL;
        }
        if (!img_ok(m, frva, 1)) return NULL;
        return m->base + frva;
    }
    return NULL;
}

/* Does a DLL with this name exist in one of the search directories? */
static int find_on_disk(const char *name, char *out, size_t outlen) {
    for (int i = 0; i < nsearch_dirs; i++) {
        snprintf(out, outlen, "%s/%s", search_dirs[i], name);
        if (access(out, R_OK) == 0) return 1;
    }
    return 0;
}

/* Bind one import to the best available target. */
static void *resolve_import(const char *dll, const char *sym, char *err, size_t errlen) {
    char want[128];
    lower_copy(want, sizeof want, dll);

    PeModule *dep = pe_find_module(want);
    if (!dep) {
        /* Not loaded yet: if the host provides the DLL, use its shims. A name the
         * host does not provide gets a logging stub. */
        if (hostlib_has_dll(want)) {
            void *data = hostlib_data(want, sym);
            if (data) return data; /* a variable: the IAT holds its address */
            void *target = hostlib_resolve(want, sym);
            return target ? target : w64_make_stub(want, sym, NULL);
        }

        /* Otherwise, a DLL file on the search path gets loaded. A file that exists
         * but fails to load is a real error. */
        char path[PATH_MAX];
        if (!find_on_disk(want, path, sizeof path)) {
            /* Unknown DLL (for example msvcrt.dll, not provided yet). */
            return w64_make_stub(want, sym, NULL);
        }
        dep = load_from_file(path, err, errlen);
        if (!dep) return NULL;
    }
    void *addr = pe_proc_address(dep, sym);
    if (addr) return addr;

    fprintf(stderr, "[xbemu] missing export %s!%s\n", dep->name, sym);
    return w64_make_stub(dep->name, sym, NULL);
}

static PeModule *load_from_file(const char *path, char *err, size_t errlen) {
    uint8_t *file = NULL;
    uint8_t *base = NULL;
    PeModule *m = NULL;
    size_t fsz = 0;
    int ok = 0;

#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); goto done; } while (0)

    FILE *f = fopen(path, "rb");
    if (!f) FAIL("cannot open %s", path);
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); FAIL("seek failed for %s", path); }
    long len = ftell(f);
    if (len < 0x40) { fclose(f); FAIL("%s is too small to be a PE image", path); }
    fsz = (size_t)len;
    rewind(f);
    file = malloc(fsz);
    if (!file || fread(file, 1, fsz, f) != fsz) { fclose(f); FAIL("read failed for %s", path); }
    fclose(f);

    if (rd16(file) != 0x5A4D) FAIL("%s is not a PE file (missing MZ)", path);
    uint32_t nt = rd32(file + 0x3C);
    if (!file_ok(fsz, nt, 24)) FAIL("bad e_lfanew in %s", path);
    if (rd32(file + nt) != 0x00004550) FAIL("bad PE signature in %s", path);

    const uint8_t *coff = file + nt + 4;
    uint16_t machine = rd16(coff), nsec = rd16(coff + 2), optsz = rd16(coff + 16);
    uint16_t chars = rd16(coff + 18);
    if (machine != PE_MACHINE_AMD64) FAIL("%s is not x86-64 (machine 0x%x)", path, machine);
    if (!file_ok(fsz, nt + 24, optsz) || optsz < 112 + 16 * 8)
        FAIL("optional header too small in %s", path);

    const uint8_t *opt = coff + 20;
    if (rd16(opt) != PE_OPT_MAGIC_PE32PLUS)
        FAIL("%s is not PE32+ (32-bit images are not supported)", path);

    uint32_t entry_rva = rd32(opt + 16);
    uint64_t image_base = rd64(opt + 24);
    uint32_t sec_align = rd32(opt + 32);
    uint32_t size_of_image = rd32(opt + 56);
    uint32_t size_of_headers = rd32(opt + 60);
    uint32_t ndirs = rd32(opt + 108);

    if (sec_align < PAGE_SIZE_BYTES) FAIL("%s: section alignment below 4 KiB unsupported", path);
    if (size_of_image == 0 || size_of_image > (1u << 30)) FAIL("%s: bad SizeOfImage", path);
    if (size_of_headers > size_of_image) FAIL("%s: SizeOfHeaders exceeds SizeOfImage", path);
    if (entry_rva && entry_rva >= size_of_image) FAIL("%s: entry point outside image", path);

    base = mmap(NULL, size_of_image, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { base = NULL; FAIL("mmap of %u bytes failed", size_of_image); }

    m = calloc(1, sizeof *m);
    if (!m) FAIL("out of memory");
    lower_copy(m->name, sizeof m->name, basename_of(path));
    m->path = realpath(path, NULL);
    if (!m->path) m->path = strdup(path);
    m->base = base;
    m->size = size_of_image;
    m->entry = entry_rva ? base + entry_rva : NULL;
    m->is_dll = (chars & PE_FILE_DLL) != 0;
    if (ndirs > 3) {
        const uint8_t *edir = opt + 112 + 3 * 8; /* IMAGE_DIRECTORY_ENTRY_EXCEPTION */
        m->exc_rva = rd32(edir);
        m->exc_size = rd32(edir + 4);
    }

    /* Register before binding imports so cyclic dependencies terminate. */
    m->next = modules;
    modules = m;
    if (!main_module) main_module = m;

    memcpy(base, file, size_of_headers < fsz ? size_of_headers : fsz);

    const uint8_t *sh = opt + optsz;
    if (!file_ok(fsz, (size_t)(sh - file), (uint64_t)nsec * 40))
        FAIL("%s: section table out of file bounds", path);

    for (unsigned i = 0; i < nsec; i++) {
        const uint8_t *s = sh + (size_t)i * 40;
        uint32_t vsize = rd32(s + 8), va = rd32(s + 12);
        uint32_t rawsz = rd32(s + 16), rawptr = rd32(s + 20);
        uint32_t span = vsize ? vsize : rawsz;
        if (!img_ok(m, va, span)) FAIL("%s: section %u outside image", path, i);
        uint32_t n = rawsz < span ? rawsz : span;
        if (n && !file_ok(fsz, rawptr, n)) FAIL("%s: section %u data outside file", path, i);
        if (n) memcpy(base + va, file + rawptr, n);
    }

    /* Base relocations, when not mapped at the preferred base. */
    uint64_t delta = (uint64_t)(uintptr_t)base - image_base;
    if (delta && ndirs > PE_DIR_BASERELOC) {
        const uint8_t *dir = opt + 112 + PE_DIR_BASERELOC * 8;
        uint32_t rrva = rd32(dir), rsz = rd32(dir + 4);
        if (rrva) {
            if (!img_ok(m, rrva, rsz)) FAIL("%s: relocation directory outside image", path);
            uint64_t off = 0;
            while (off + 8 <= rsz) {
                uint32_t page = rd32(base + rrva + off);
                uint32_t blk = rd32(base + rrva + off + 4);
                if (blk < 8 || off + blk > rsz) FAIL("%s: malformed relocation block", path);
                for (uint32_t e = 8; e + 2 <= blk; e += 2) {
                    uint16_t ent = rd16(base + rrva + off + e);
                    unsigned type = ent >> 12;
                    uint32_t where = page + (ent & 0xFFF);
                    switch (type) {
                    case PE_REL_ABSOLUTE:
                        break;
                    case PE_REL_HIGHLOW:
                        if (!img_ok(m, where, 4)) FAIL("%s: reloc outside image", path);
                        { uint32_t v; memcpy(&v, base + where, 4); v += (uint32_t)delta;
                          memcpy(base + where, &v, 4); }
                        break;
                    case PE_REL_DIR64:
                        if (!img_ok(m, where, 8)) FAIL("%s: reloc outside image", path);
                        { uint64_t v; memcpy(&v, base + where, 8); v += delta;
                          memcpy(base + where, &v, 8); }
                        break;
                    default:
                        FAIL("%s: unsupported relocation type %u", path, type);
                    }
                }
                off += blk;
            }
        }
    }

    /* Imports. */
    if (ndirs > PE_DIR_IMPORT) {
        const uint8_t *dir = opt + 112 + PE_DIR_IMPORT * 8;
        uint32_t irva = rd32(dir);
        if (irva) {
            for (uint64_t d = irva;; d += 20) {
                if (!img_ok(m, d, 20)) FAIL("%s: import descriptor outside image", path);
                const uint8_t *desc = base + d;
                uint32_t oft = rd32(desc), name_rva = rd32(desc + 12), ft = rd32(desc + 16);
                if (!oft && !name_rva && !ft) break;
                if (!ft || !img_ok(m, name_rva, 1)) FAIL("%s: malformed import descriptor", path);

                uint32_t lookup = oft ? oft : ft;
                uint64_t dll_max = m->size - name_rva;
                const char *dll = (const char *)(base + name_rva);
                if (strnlen(dll, dll_max) == dll_max) FAIL("%s: unterminated import DLL name", path);

                for (uint64_t k = 0;; k++) {
                    if (!img_ok(m, lookup + k * 8, 8) || !img_ok(m, ft + k * 8, 8))
                        FAIL("%s: import thunk table outside image", path);
                    uint64_t ilt = rd64(base + lookup + k * 8);
                    if (!ilt) break;

                    char ordname[16];
                    const char *sym;
                    if (ilt & PE_ORDINAL_FLAG64) {
                        snprintf(ordname, sizeof ordname, "#%u", (unsigned)(ilt & 0xFFFF));
                        sym = ordname;
                    } else {
                        uint32_t hint_rva = (uint32_t)ilt;
                        if (!img_ok(m, hint_rva, 3)) FAIL("%s: import name outside image", path);
                        uint64_t smax = m->size - (hint_rva + 2);
                        sym = (const char *)(base + hint_rva + 2);
                        if (strnlen(sym, smax) == smax) FAIL("%s: unterminated import name", path);
                    }

                    void *target = resolve_import(dll, sym, err, errlen);
                    if (!target) FAIL("%s: cannot bind %s!%s", path, dll, sym);
                    uint64_t addr = (uint64_t)(uintptr_t)target;
                    memcpy(base + ft + k * 8, &addr, 8);
                }
            }
        }
    }

    /* Protect headers read-only, then each section per its characteristics. */
    size_t hdr_len = (size_of_headers + PAGE_SIZE_BYTES - 1) & ~(size_t)(PAGE_SIZE_BYTES - 1);
    if (mprotect(base, hdr_len, PROT_READ) != 0) FAIL("%s: mprotect(headers) failed", path);
    for (unsigned i = 0; i < nsec; i++) {
        const uint8_t *s = sh + (size_t)i * 40;
        uint32_t vsize = rd32(s + 8), va = rd32(s + 12), rawsz = rd32(s + 16);
        uint32_t ch = rd32(s + 36);
        uint32_t span = vsize ? vsize : rawsz;
        uint32_t len = (span + PAGE_SIZE_BYTES - 1) & ~(PAGE_SIZE_BYTES - 1);
        if (!len) continue;
        if (!img_ok(m, va, len)) len = (uint32_t)(m->size - va);
        if (mprotect(base + va, len, sec_prot(ch)) != 0) FAIL("%s: mprotect(section %u) failed", path, i);
    }

    /* DllMain: called after imports are bound and sections are protected. */
    if (m->is_dll && m->entry) {
        if (!xbemu_call_w64(m->entry, (uint64_t)(uintptr_t)base, DLL_PROCESS_ATTACH, 0, 0))
            FAIL("%s: DllMain returned FALSE", path);
    }
    ok = 1;

done:
    free(file);
    if (!ok && m) {
        /* Unlink and unmap. Modules already loaded through this one are left registered. */
        if (main_module == m) main_module = NULL;
        if (modules == m) modules = m->next;
        else for (PeModule *p = modules; p; p = p->next)
            if (p->next == m) { p->next = m->next; break; }
        if (m->base) munmap(m->base, m->size);
        free(m->path);
        free(m);
        m = NULL;
    }
    return m;
#undef FAIL
}

PeModule *pe_load_module(const char *path_or_name, char *err, size_t errlen) {
    PeModule *existing = pe_find_module(path_or_name);
    if (existing) return existing;

    if (strchr(path_or_name, '/')) return load_from_file(path_or_name, err, errlen);

    for (int i = 0; i < nsearch_dirs; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", search_dirs[i], path_or_name);
        if (access(path, R_OK) == 0) return load_from_file(path, err, errlen);
    }
    snprintf(err, errlen, "module %s not found", path_or_name);
    return NULL;
}

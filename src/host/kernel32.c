/*
 * kernel32.dll shims: core process, time, TLS, and error state.
 * Sync primitives are in kernel32_sync.c, module loading in kernel32_loader.c.
 * Guest pointers are host pointers (single shared address space).
 */
#define _DEFAULT_SOURCE
#include "kernel32_impl.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ---- last error (per thread) ---- */

static __thread uint32_t last_error;

void k32_set_last_error(uint32_t err) { last_error = err; }
uint32_t k32_get_last_error(void) { return last_error; }

void k32_wide_to_ascii(const uint16_t *src, char *dst, size_t dstlen) {
    size_t i = 0;
    if (dstlen == 0) return;
    for (; src && src[i] && i + 1 < dstlen; i++)
        dst[i] = src[i] < 0x80 ? (char)src[i] : '?';
    dst[i] = 0;
}

/* ---- process ---- */

/* VOID ExitProcess(UINT) */
static uint64_t k32_ExitProcess(W64Ctx *c) {
    fflush(stdout);
    fflush(stderr);
    exit((int)(uint32_t)w64_arg(c, 0));
}

static uint64_t k32_GetLastError(W64Ctx *c) {
    (void)c;
    return last_error;
}

static uint64_t k32_SetLastError(W64Ctx *c) {
    k32_set_last_error((uint32_t)w64_arg(c, 0));
    return 0;
}

static uint64_t k32_GetCurrentProcess(W64Ctx *c) { (void)c; return (uint64_t)-1; }
static uint64_t k32_GetCurrentThread(W64Ctx *c)  { (void)c; return (uint64_t)-2; }

static uint64_t k32_GetCurrentProcessId(W64Ctx *c) {
    (void)c;
    return (uint32_t)getpid();
}

static uint64_t k32_GetCurrentThreadId(W64Ctx *c) {
    (void)c;
    return k32_current_tid();
}

/* BOOL ProcessIdToSessionId(DWORD pid, DWORD *session) */
static uint64_t k32_ProcessIdToSessionId(W64Ctx *c) {
    uint32_t *session = (uint32_t *)(uintptr_t)w64_arg(c, 1);
    if (session) *session = 1;
    return 1;
}

/* ---- time ---- */

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ULONGLONG GetTickCount64(void) */
static uint64_t k32_GetTickCount64(W64Ctx *c) {
    (void)c;
    return monotonic_ns() / 1000000ull;
}

/* VOID Sleep(DWORD ms) */
static uint64_t k32_Sleep(W64Ctx *c) {
    uint32_t ms = (uint32_t)w64_arg(c, 0);
    if (ms == WIN_INFINITE) {
        for (;;) sleep(3600);
    }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    return 0;
}

/* BOOL SwitchToThread(void) */
static uint64_t k32_SwitchToThread(W64Ctx *c) {
    (void)c;
    sched_yield();
    return 1;
}

/* BOOL QueryPerformanceCounter(LARGE_INTEGER *) */
static uint64_t k32_QueryPerformanceCounter(W64Ctx *c) {
    int64_t *out = (int64_t *)(uintptr_t)w64_arg(c, 0);
    if (out) *out = (int64_t)monotonic_ns();
    return 1;
}

/* BOOL QueryPerformanceFrequency(LARGE_INTEGER *) */
static uint64_t k32_QueryPerformanceFrequency(W64Ctx *c) {
    int64_t *out = (int64_t *)(uintptr_t)w64_arg(c, 0);
    if (out) *out = 1000000000; /* 1 GHz: counter is in nanoseconds */
    return 1;
}

/* VOID GetSystemTimeAsFileTime(LPFILETIME): 100 ns units since 1601-01-01 UTC. */
static uint64_t k32_GetSystemTimeAsFileTime(W64Ctx *c) {
    uint64_t *out = (uint64_t *)(uintptr_t)w64_arg(c, 0);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ft = (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100
                  + 116444736000000000ull;
    if (out) *out = ft;
    return 0;
}

/* DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION): UTC, nothing else known. */
static uint64_t k32_GetTimeZoneInformation(W64Ctx *c) {
    void *tzi = (void *)(uintptr_t)w64_arg(c, 0);
    if (tzi) memset(tzi, 0, 172); /* sizeof(TIME_ZONE_INFORMATION) */
    return 0; /* TIME_ZONE_ID_UNKNOWN */
}

/* ---- system info ---- */

/* VOID GetSystemInfo(LPSYSTEM_INFO): 48-byte struct for x64. */
static uint64_t k32_GetSystemInfo(W64Ctx *c) {
    uint8_t *si = (uint8_t *)(uintptr_t)w64_arg(c, 0);
    if (!si) return 0;
    memset(si, 0, 48);
    uint16_t arch = 9; /* PROCESSOR_ARCHITECTURE_AMD64 */
    uint32_t page = 4096;
    uint64_t minaddr = 0x10000, maxaddr = 0x7FFFFFFEFFFFull, mask = 0;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    for (long i = 0; i < ncpu && i < 64; i++) mask |= 1ull << i;
    uint32_t gran = 65536;
    memcpy(si + 0, &arch, 2);
    memcpy(si + 4, &page, 4);
    memcpy(si + 8, &minaddr, 8);
    memcpy(si + 16, &maxaddr, 8);
    memcpy(si + 24, &mask, 8);
    uint32_t n = (uint32_t)ncpu;
    memcpy(si + 32, &n, 4);
    memcpy(si + 40, &gran, 4);
    return 0;
}

/* ---- memory protection ---- */

/* Win32 PAGE_* protection to POSIX PROT_*. */
static int win_prot_to_posix(uint32_t p) {
    switch (p & 0xFF) {
    case 0x01: return PROT_NONE;                            /* PAGE_NOACCESS */
    case 0x02: return PROT_READ;                            /* PAGE_READONLY */
    case 0x04: return PROT_READ | PROT_WRITE;               /* PAGE_READWRITE */
    case 0x08: return PROT_READ | PROT_WRITE;               /* PAGE_WRITECOPY */
    case 0x10: return PROT_EXEC;                            /* PAGE_EXECUTE */
    case 0x20: return PROT_READ | PROT_EXEC;                /* PAGE_EXECUTE_READ */
    case 0x40: return PROT_READ | PROT_WRITE | PROT_EXEC;   /* PAGE_EXECUTE_READWRITE */
    case 0x80: return PROT_READ | PROT_WRITE | PROT_EXEC;   /* PAGE_EXECUTE_WRITECOPY */
    default:   return -1;
    }
}

static uint32_t maps_prot(const char *perm);

/* Current Win32 protection of the page containing addr, read from /proc/self/maps.
 * Returns 0 when the address is not mapped. */
static uint32_t page_protection(uintptr_t addr) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uint32_t prot = 0;
    while (fgets(line, sizeof line, f)) {
        uintptr_t lo, hi;
        char p[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, p) == 3 && addr >= lo && addr < hi) {
            prot = maps_prot(p);
            break;
        }
    }
    fclose(f);
    return prot;
}

/* BOOL VirtualProtect(LPVOID, SIZE_T, DWORD new, PDWORD old): *old is the protection of the
 * first page before the change. */
static uint64_t k32_VirtualProtect(W64Ctx *c) {
    uintptr_t addr = (uintptr_t)w64_arg(c, 0);
    size_t size = (size_t)w64_arg(c, 1);
    uint32_t newp = (uint32_t)w64_arg(c, 2);
    uint32_t *old = (uint32_t *)(uintptr_t)w64_arg(c, 3);

    int prot = win_prot_to_posix(newp);
    if (prot < 0) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }

    uintptr_t page = addr & ~(uintptr_t)4095;
    size_t len = ((addr - page) + size + 4095) & ~(size_t)4095;
    uint32_t before = page_protection(page);
    if (mprotect((void *)page, len, prot) != 0) {
        k32_set_last_error(WIN_ERROR_ACCESS_DENIED);
        return 0;
    }
    if (old) *old = before ? before : newp;
    return 1;
}

/* ---- memory queries ---- */

/* Win32 PAGE_* for a /proc/self/maps permission string such as "rwxp". */
static uint32_t maps_prot(const char *perm) {
    int r = perm[0] == 'r', w = perm[1] == 'w', x = perm[2] == 'x';
    if (x && w) return 0x40; /* PAGE_EXECUTE_READWRITE */
    if (x) return r ? 0x20 : 0x10; /* PAGE_EXECUTE_READ / PAGE_EXECUTE */
    if (w) return 0x04;      /* PAGE_READWRITE */
    if (r) return 0x02;      /* PAGE_READONLY */
    return 0x01;             /* PAGE_NOACCESS */
}

/*
 * SIZE_T VirtualQuery(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T)
 * Reads the region from /proc/self/maps. MEMORY_BASIC_INFORMATION is 48 bytes on x64.
 */
static uint64_t k32_VirtualQuery(W64Ctx *c) {
    uintptr_t addr = (uintptr_t)w64_arg(c, 0);
    uint8_t *mbi = (uint8_t *)(uintptr_t)w64_arg(c, 1);
    size_t len = (size_t)w64_arg(c, 2);
    if (!mbi || len < 48) { k32_set_last_error(87); return 0; }

    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { k32_set_last_error(WIN_ERROR_NOT_SUPPORTED); return 0; }
    char line[512];
    uintptr_t start = 0, end = 0;
    char perm[8] = "";
    while (fgets(line, sizeof line, f)) {
        uintptr_t lo, hi;
        char p[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, p) == 3 && addr >= lo && addr < hi) {
            start = lo; end = hi;
            memcpy(perm, p, sizeof perm);
            break;
        }
    }
    fclose(f);
    if (!end) { k32_set_last_error(487); return 0; } /* ERROR_INVALID_ADDRESS */

    uint64_t base = start, size = end - start;
    uint32_t prot = maps_prot(perm), alloc_prot = prot, state = 0x1000 /* MEM_COMMIT */;
    uint32_t type = 0x20000; /* MEM_PRIVATE */
    if (prot == 0x01) state = 0x10000; /* MEM_FREE for PROT_NONE mappings */
    memset(mbi, 0, 48);
    memcpy(mbi + 0, &base, 8);      /* BaseAddress */
    memcpy(mbi + 8, &base, 8);      /* AllocationBase */
    memcpy(mbi + 16, &alloc_prot, 4);
    memcpy(mbi + 24, &size, 8);     /* RegionSize */
    memcpy(mbi + 32, &state, 4);    /* State */
    memcpy(mbi + 36, &prot, 4);     /* Protect */
    memcpy(mbi + 40, &type, 4);     /* Type */
    return 48;
}

/* LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(filter): stored, never called. */
static void *unhandled_filter;
static uint64_t k32_SetUnhandledExceptionFilter(W64Ctx *c) {
    void *prev = unhandled_filter;
    unhandled_filter = (void *)(uintptr_t)w64_arg(c, 0);
    return (uint64_t)(uintptr_t)prev;
}

/* ---- thread-local storage ---- */

#define TLS_SLOTS 64
#define TLS_OUT_OF_INDEXES 0xFFFFFFFFu

static uint8_t tls_used[TLS_SLOTS];
static __thread void *tls_values[TLS_SLOTS];

/* DWORD TlsAlloc(void) */
static uint64_t k32_TlsAlloc(W64Ctx *c) {
    (void)c;
    for (uint32_t i = 0; i < TLS_SLOTS; i++) {
        if (!tls_used[i]) {
            tls_used[i] = 1;
            return i;
        }
    }
    return TLS_OUT_OF_INDEXES;
}

/* BOOL TlsFree(DWORD) */
static uint64_t k32_TlsFree(W64Ctx *c) {
    uint32_t i = (uint32_t)w64_arg(c, 0);
    if (i >= TLS_SLOTS || !tls_used[i]) return 0;
    tls_used[i] = 0;
    return 1;
}

/* LPVOID TlsGetValue(DWORD) */
static uint64_t k32_TlsGetValue(W64Ctx *c) {
    uint32_t i = (uint32_t)w64_arg(c, 0);
    if (i >= TLS_SLOTS) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }
    k32_set_last_error(0);
    return (uint64_t)(uintptr_t)tls_values[i];
}

/* BOOL TlsSetValue(DWORD, LPVOID) */
static uint64_t k32_TlsSetValue(W64Ctx *c) {
    uint32_t i = (uint32_t)w64_arg(c, 0);
    if (i >= TLS_SLOTS || !tls_used[i]) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }
    tls_values[i] = (void *)(uintptr_t)w64_arg(c, 1);
    return 1;
}

/* ---- misc ---- */

/* BOOL IsDBCSLeadByteEx(UINT codepage, BYTE ch): no DBCS code pages in use. */
static uint64_t k32_IsDBCSLeadByteEx(W64Ctx *c) {
    (void)c;
    return 0;
}

/* DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size) */
static uint64_t k32_GetEnvironmentVariableW(W64Ctx *c) {
    char name[256];
    k32_wide_to_ascii((const uint16_t *)(uintptr_t)w64_arg(c, 0), name, sizeof name);
    const char *v = getenv(name);
    if (!v) { k32_set_last_error(203); return 0; } /* ERROR_ENVVAR_NOT_FOUND */
    uint16_t *buf = (uint16_t *)(uintptr_t)w64_arg(c, 1);
    uint32_t size = (uint32_t)w64_arg(c, 2);
    size_t n = strlen(v);
    if (buf && size > n) {
        for (size_t i = 0; i <= n; i++) buf[i] = (uint16_t)(unsigned char)v[i];
    }
    return n;
}

/* UINT GetSystemDirectoryA: there is no Windows system directory here. */
static uint64_t k32_GetSystemDirectoryA(W64Ctx *c) {
    (void)c;
    k32_set_last_error(WIN_ERROR_NOT_SUPPORTED);
    return 0;
}

/* ---- code page conversion ----
 * CP_ACP (0) and CP_UTF8 (65001) are treated as UTF-8. Other code pages are treated as
 * Latin-1 (one byte per character), which is not correct for every code page. */

#define CP_ACP  0u
#define CP_UTF8 65001u
#define ERROR_INSUFFICIENT_BUFFER 122u

static int cp_is_utf8(uint32_t cp) { return cp == CP_ACP || cp == CP_UTF8; }

/* Decode n bytes of UTF-8 into UTF-16 units. Writes at most cap units; returns the count needed. */
static size_t utf8_to_utf16(const uint8_t *s, size_t n, uint16_t *dst, size_t cap) {
    size_t out = 0;
#define EMIT_UNIT(v) do { if (dst && out < cap) dst[out] = (uint16_t)(v); out++; } while (0)
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        size_t len = 1;
        uint8_t b = s[i];
        if (b < 0x80) { cp = b; }
        else if ((b >> 5) == 6 && i + 1 < n) { cp = ((b & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu); len = 2; }
        else if ((b >> 4) == 14 && i + 2 < n) {
            cp = ((b & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu); len = 3;
        } else if ((b >> 3) == 30 && i + 3 < n) {
            cp = ((b & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) |
                 ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
            len = 4;
        } else { cp = 0xFFFD; }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            EMIT_UNIT(0xD800 + (cp >> 10));
            EMIT_UNIT(0xDC00 + (cp & 0x3FF));
        } else {
            EMIT_UNIT(cp);
        }
    }
#undef EMIT_UNIT
    return out;
}

/* int MultiByteToWideChar(UINT cp, DWORD flags, LPCCH src, int srclen, LPWSTR dst, int dstlen) */
static uint64_t k32_MultiByteToWideChar(W64Ctx *c) {
    uint32_t cp = (uint32_t)w64_arg(c, 0);
    const uint8_t *src = (const uint8_t *)(uintptr_t)w64_arg(c, 2);
    int32_t srclen = (int32_t)(uint32_t)w64_arg(c, 3);
    uint16_t *dst = (uint16_t *)(uintptr_t)w64_arg(c, 4);
    int32_t dstlen = (int32_t)(uint32_t)w64_arg(c, 5);

    if (!src || srclen == 0) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }
    size_t n = srclen < 0 ? strlen((const char *)src) + 1 : (size_t)srclen;

    size_t units;
    if (cp_is_utf8(cp)) {
        units = utf8_to_utf16(src, n, NULL, 0);
        if (dst && dstlen > 0) {
            if ((size_t)dstlen < units) { k32_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
            utf8_to_utf16(src, n, dst, (size_t)dstlen);
        }
    } else {
        units = n;
        if (dst && dstlen > 0) {
            if ((size_t)dstlen < units) { k32_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
            for (size_t i = 0; i < n; i++) dst[i] = src[i];
        }
    }
    return (uint32_t)units;
}

/* int WideCharToMultiByte(UINT cp, DWORD flags, LPCWCH src, int srclen, LPSTR dst, int dstlen,
 *                         LPCCH default, LPBOOL used_default) */
static uint64_t k32_WideCharToMultiByte(W64Ctx *c) {
    uint32_t cp = (uint32_t)w64_arg(c, 0);
    const uint16_t *src = (const uint16_t *)(uintptr_t)w64_arg(c, 2);
    int32_t srclen = (int32_t)(uint32_t)w64_arg(c, 3);
    uint8_t *dst = (uint8_t *)(uintptr_t)w64_arg(c, 4);
    int32_t dstlen = (int32_t)(uint32_t)w64_arg(c, 5);

    if (!src || srclen == 0) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }
    size_t n = srclen;
    if (srclen < 0) { n = 0; while (src[n]) n++; n++; }

    /* Encode into a temporary buffer first, so the output size is known. */
    size_t cap = n * 4 + 1;
    uint8_t *tmp = malloc(cap);
    if (!tmp) { k32_set_last_error(8); return 0; }
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t u = src[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n && src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) {
            u = 0x10000 + ((u - 0xD800) << 10) + (src[i + 1] - 0xDC00);
            i++;
        }
        if (cp_is_utf8(cp)) {
            if (u < 0x80) tmp[out++] = (uint8_t)u;
            else if (u < 0x800) { tmp[out++] = (uint8_t)(0xC0 | (u >> 6)); tmp[out++] = (uint8_t)(0x80 | (u & 0x3F)); }
            else if (u < 0x10000) {
                tmp[out++] = (uint8_t)(0xE0 | (u >> 12));
                tmp[out++] = (uint8_t)(0x80 | ((u >> 6) & 0x3F));
                tmp[out++] = (uint8_t)(0x80 | (u & 0x3F));
            } else {
                tmp[out++] = (uint8_t)(0xF0 | (u >> 18));
                tmp[out++] = (uint8_t)(0x80 | ((u >> 12) & 0x3F));
                tmp[out++] = (uint8_t)(0x80 | ((u >> 6) & 0x3F));
                tmp[out++] = (uint8_t)(0x80 | (u & 0x3F));
            }
        } else {
            tmp[out++] = u < 0x100 ? (uint8_t)u : '?';
        }
    }
    size_t result = out;
    if (dst && dstlen > 0) {
        if ((size_t)dstlen < out) { free(tmp); k32_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        memcpy(dst, tmp, out);
    }
    free(tmp);
    return (uint32_t)result;
}

const HostExport kernel32_exports[] = {
    { "MultiByteToWideChar",         k32_MultiByteToWideChar         },
    { "WideCharToMultiByte",         k32_WideCharToMultiByte         },
    { "ExitProcess",                 k32_ExitProcess                 },
    { "GetLastError",                k32_GetLastError                },
    { "SetLastError",                k32_SetLastError                },
    { "GetCurrentProcess",           k32_GetCurrentProcess           },
    { "GetCurrentThread",            k32_GetCurrentThread            },
    { "GetCurrentProcessId",         k32_GetCurrentProcessId         },
    { "GetCurrentThreadId",          k32_GetCurrentThreadId          },
    { "ProcessIdToSessionId",        k32_ProcessIdToSessionId        },
    { "GetTickCount64",              k32_GetTickCount64              },
    { "Sleep",                       k32_Sleep                       },
    { "SwitchToThread",              k32_SwitchToThread              },
    { "QueryPerformanceCounter",     k32_QueryPerformanceCounter     },
    { "QueryPerformanceFrequency",   k32_QueryPerformanceFrequency   },
    { "GetSystemTimeAsFileTime",     k32_GetSystemTimeAsFileTime     },
    { "GetTimeZoneInformation",      k32_GetTimeZoneInformation      },
    { "GetSystemInfo",               k32_GetSystemInfo               },
    { "VirtualProtect",              k32_VirtualProtect              },
    { "TlsAlloc",                    k32_TlsAlloc                    },
    { "TlsFree",                     k32_TlsFree                     },
    { "TlsGetValue",                 k32_TlsGetValue                 },
    { "TlsSetValue",                 k32_TlsSetValue                 },
    { "IsDBCSLeadByteEx",            k32_IsDBCSLeadByteEx            },
    { "GetEnvironmentVariableW",     k32_GetEnvironmentVariableW     },
    { "GetSystemDirectoryA",         k32_GetSystemDirectoryA         },
    { "VirtualQuery",                k32_VirtualQuery                },
    { "SetUnhandledExceptionFilter", k32_SetUnhandledExceptionFilter },
    { NULL, NULL },
};

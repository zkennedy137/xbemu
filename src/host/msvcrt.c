/*
 * msvcrt.dll shims: C runtime functions used by DXVK.
 *
 * Most functions forward to glibc. The ABI matches once arguments are read from
 * the Win64 registers, so this is a thin layer. Differences:
 * - wchar_t is 16 bits on Windows and 32 bits on glibc, so wide-string functions
 *   are written by hand.
 * - Windows FILE* values from __iob_func map to host stdin/stdout/stderr. Other
 *   FILE* values are opaque host handles and pass through.
 * - printf-style formatting covers integers, strings, and pointers. Floating-point
 *   conversions print "?" for now.
 */
#define _GNU_SOURCE
#include "kernel32_impl.h"
#include "hostlib.h"
#include "paths.h"

#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define A(n) w64_arg(c, (n))
#define P(n, T) ((T)(uintptr_t)w64_arg(c, (n)))
#define RET_PTR(p) ((uint64_t)(uintptr_t)(p))

/* ---- Windows FILE* mapping ---- */

/* MSVC's _iobuf is 48 bytes on x64. __iob_func returns an array of three. */
#define IOB_STRIDE 48
static uint8_t iob_area[3 * IOB_STRIDE];

static FILE *host_file(const void *p) {
    const uint8_t *b = p;
    if (b >= iob_area && b < iob_area + sizeof iob_area) {
        size_t idx = (size_t)(b - iob_area) / IOB_STRIDE;
        return idx == 0 ? stdin : idx == 1 ? stdout : stderr;
    }
    return (FILE *)(uintptr_t)p;
}

/* ---- UTF-16 helpers (Windows wchar_t is 16 bits) ---- */

static size_t wlen16(const uint16_t *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Convert a UTF-16 string to a malloc'd UTF-8 string. */
static char *utf16_to_utf8(const uint16_t *s) {
    if (!s) return NULL;
    size_t n = wlen16(s);
    char *out = malloc(n * 4 + 1);
    if (!out) return NULL;
    char *o = out;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            *o++ = (char)cp;
        } else if (cp < 0x800) {
            *o++ = (char)(0xC0 | (cp >> 6));
            *o++ = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            *o++ = (char)(0xE0 | (cp >> 12));
            *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
            *o++ = (char)(0x80 | (cp & 0x3F));
        } else {
            *o++ = (char)(0xF0 | (cp >> 18));
            *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
            *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
            *o++ = (char)(0x80 | (cp & 0x3F));
        }
    }
    *o = 0;
    return out;
}

/* Convert a UTF-16 string to ASCII for file names and options. */
static char *utf16_to_ascii(const uint16_t *s) {
    if (!s) return NULL;
    size_t n = wlen16(s);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) out[i] = s[i] < 0x80 ? (char)s[i] : '?';
    out[n] = 0;
    return out;
}

/* ---- printf-style formatting ----
 * args[] holds the variadic arguments in Win64 order, one 8-byte slot each.
 * Returns a malloc'd narrow (UTF-8) string, or NULL on failure. */

static uint64_t next_arg(const uint64_t *args, size_t nargs, size_t *pos) {
    return *pos < nargs ? args[(*pos)++] : 0;
}

/* Format one integer with the flags and width from spec. */
static int fmt_int(char *buf, size_t cap, const char *spec, char conv, int is64, uint64_t raw) {
    char cf[96];
    int is_signed = (conv == 'd' || conv == 'i');
    const char *lm = is64 ? "ll" : "";
    if (is_signed) {
        if (is64) {
            snprintf(cf, sizeof cf, "%s%sd", spec, lm);
            snprintf(buf, cap, cf, (long long)raw);
        } else {
            snprintf(cf, sizeof cf, "%sd", spec);
            snprintf(buf, cap, cf, (int)raw);
        }
    } else {
        char c2 = (conv == 'i') ? 'd' : conv;
        if (is64) {
            snprintf(cf, sizeof cf, "%s%s%c", spec, lm, c2);
            snprintf(buf, cap, cf, (unsigned long long)raw);
        } else {
            snprintf(cf, sizeof cf, "%s%c", spec, c2);
            snprintf(buf, cap, cf, (unsigned int)raw);
        }
    }
    return (int)strlen(buf);
}

static char *do_format(const void *fmt_raw, int wide_format, const uint64_t *args,
                       const uint64_t *fargs, size_t nargs) {
    /* fargs: the same arguments as they sit in xmm registers (positions 0-3), or NULL. */
    char *fmt = wide_format ? utf16_to_utf8(fmt_raw) : strdup(fmt_raw);
    if (!fmt) return NULL;

    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    if (!out) { free(fmt); return NULL; }
    out[0] = 0;
    size_t pos = 0;

    /* Append n bytes from s to out, growing as needed. Returns 0 on out-of-memory. */
#define APPEND(s, n) do { \
        size_t _n = (n); \
        if (len + _n + 1 > cap) { \
            while (len + _n + 1 > cap) cap *= 2; \
            char *_t = realloc(out, cap); \
            if (!_t) goto fail; \
            out = _t; \
        } \
        memcpy(out + len, (s), _n); len += _n; out[len] = 0; \
    } while (0)

    for (const char *p = fmt; *p;) {
        if (*p != '%') { APPEND(p, 1); p++; continue; }
        p++;
        if (*p == '%') { APPEND("%", 1); p++; continue; }

        /* Build the C spec: flags, width, precision. */
        char spec[96];
        size_t sl = 0;
        spec[sl++] = '%';
        while (*p && strchr("-+ 0#", *p) && sl < 16) spec[sl++] = *p++;
        if (*p == '*') {
            int w = (int)(uint32_t)next_arg(args, nargs, &pos);
            sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%d", w);
            p++;
        }
        while (isdigit((unsigned char)*p) && sl < 40) spec[sl++] = *p++;
        if (*p == '.') {
            spec[sl++] = *p++;
            if (*p == '*') {
                int pr = (int)(uint32_t)next_arg(args, nargs, &pos);
                sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%d", pr);
                p++;
            }
            while (isdigit((unsigned char)*p) && sl < 60) spec[sl++] = *p++;
        }
        spec[sl] = 0;

        /* Length modifiers. */
        int is64 = 0, wide_arg = 0;
        for (;;) {
            if (*p == 'l') {
                if (p[1] == 'l') { is64 = 1; p += 2; }
                else { wide_arg = 1; p++; }
            } else if (*p == 'h') { p++; if (*p == 'h') p++; }
            else if (*p == 'z' || *p == 'j' || *p == 't') { is64 = 1; p++; }
            else if (*p == 'I' && p[1] == '6' && p[2] == '4') { is64 = 1; p += 3; }
            else if (*p == 'I' || *p == 'w') p++;
            else break;
        }
        char conv = *p ? *p++ : 0;

        char buf[1024];
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            uint64_t raw = next_arg(args, nargs, &pos);
            int n = fmt_int(buf, sizeof buf, spec, conv, is64, raw);
            APPEND(buf, (size_t)n);
            break;
        }
        case 'c': {
            uint64_t raw = next_arg(args, nargs, &pos);
            char cf[112];
            snprintf(cf, sizeof cf, "%sc", spec);
            int n = snprintf(buf, sizeof buf, cf, (int)(uint8_t)raw);
            APPEND(buf, (size_t)n);
            break;
        }
        case 'p': {
            uint64_t raw = next_arg(args, nargs, &pos);
            int n = snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)raw);
            APPEND(buf, (size_t)n);
            break;
        }
        case 's': case 'S': {
            /* %ls and (in wide format) %s take UTF-16. Otherwise the argument is narrow. */
            int want_wide = (conv == 's') ? (wide_arg || wide_format) : (!wide_format);
            const void *ptr = (const void *)(uintptr_t)next_arg(args, nargs, &pos);
            char *conv_str = NULL;
            const char *s = "(null)";
            if (ptr) {
                if (want_wide) { conv_str = utf16_to_utf8(ptr); s = conv_str ? conv_str : "(null)"; }
                else s = ptr;
            }
            char cf[112];
            snprintf(cf, sizeof cf, "%ss", spec);
            int n = snprintf(buf, sizeof buf, cf, s);
            if (n < 0) n = 0;
            if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
            APPEND(buf, (size_t)n);
            free(conv_str);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            /* A double argument: Win64 passes it in xmm for its position, otherwise the raw slot. */
            size_t idx = pos;
            next_arg(args, nargs, &pos);
            uint64_t bits = idx < nargs ? (fargs ? fargs[idx] : args[idx]) : 0;
            double d;
            memcpy(&d, &bits, 8);
            char cf[112];
            snprintf(cf, sizeof cf, "%s%c", spec, conv);
            int n = snprintf(buf, sizeof buf, cf, d);
            if (n < 0) n = 0;
            if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
            APPEND(buf, (size_t)n);
            break;
        }
        case 'n':
            next_arg(args, nargs, &pos);
            break;
        default:
            /* Unknown conversion: copy it through unchanged. */
            if (conv) APPEND(&conv, 1);
            break;
        }
    }
#undef APPEND
    free(fmt);
    return out;

fail:
    free(fmt);
    free(out);
    return NULL;
}

/* Copy the Win64 variadic arguments that start at position `first` into out. */
static size_t gather_varargs(const W64Ctx *c, unsigned first, uint64_t *out, uint64_t *fout, size_t max) {
    for (size_t i = 0; i < max; i++) {
        unsigned pos = first + (unsigned)i;
        out[i] = w64_arg(c, pos);
        /* Positions 0-3 are also passed in xmm registers, which is where a double is read. */
        fout[i] = pos < 4 ? c->xmm[pos][0] : out[i];
    }
    return max;
}

#define VA_MAX 24

/* ---- memory ---- */

/* void *malloc(size_t) */
static uint64_t m_malloc(W64Ctx *c) { return RET_PTR(malloc(A(0))); }
/* void *calloc(size_t, size_t) */
static uint64_t m_calloc(W64Ctx *c) { return RET_PTR(calloc(A(0), A(1))); }
/* void *realloc(void *, size_t) */
static uint64_t m_realloc(W64Ctx *c) { return RET_PTR(realloc(P(0, void *), A(1))); }
/* void free(void *) */
static uint64_t m_free(W64Ctx *c) { free(P(0, void *)); return 0; }

/* void *_aligned_malloc(size_t size, size_t alignment) */
static uint64_t m_aligned_malloc(W64Ctx *c) {
    size_t size = A(0), align = A(1);
    if (align < sizeof(void *)) align = sizeof(void *);
    if (align & (align - 1)) return 0;
    size_t rounded = (size + align - 1) & ~(align - 1);
    if (!rounded) rounded = align;
    return RET_PTR(aligned_alloc(align, rounded));
}
/* void _aligned_free(void *) */
static uint64_t m_aligned_free(W64Ctx *c) { free(P(0, void *)); return 0; }

/* void *memcpy / memmove / memset / memcmp / memchr */
static uint64_t m_memcpy(W64Ctx *c) { return RET_PTR(memcpy(P(0, void *), P(1, const void *), A(2))); }
static uint64_t m_memmove(W64Ctx *c) { return RET_PTR(memmove(P(0, void *), P(1, const void *), A(2))); }
static uint64_t m_memset(W64Ctx *c) { return RET_PTR(memset(P(0, void *), (int)A(1), A(2))); }
static uint64_t m_memcmp(W64Ctx *c) { return (uint32_t)memcmp(P(0, const void *), P(1, const void *), A(2)); }
static uint64_t m_memchr(W64Ctx *c) { return RET_PTR(memchr(P(0, const void *), (int)A(1), A(2))); }

/* ---- narrow strings ---- */

static uint64_t m_strlen(W64Ctx *c) { return strlen(P(0, const char *)); }
static uint64_t m_strcmp(W64Ctx *c) { return (uint32_t)strcmp(P(0, const char *), P(1, const char *)); }
static uint64_t m_strncmp(W64Ctx *c) { return (uint32_t)strncmp(P(0, const char *), P(1, const char *), A(2)); }
static uint64_t m_strchr(W64Ctx *c) { return RET_PTR(strchr(P(0, const char *), (int)A(1))); }
static uint64_t m_strstr(W64Ctx *c) { return RET_PTR(strstr(P(0, const char *), P(1, const char *))); }
static uint64_t m_strncpy(W64Ctx *c) { return RET_PTR(strncpy(P(0, char *), P(1, const char *), A(2))); }
static uint64_t m_strcoll(W64Ctx *c) { return (uint32_t)strcoll(P(0, const char *), P(1, const char *)); }
static uint64_t m_strerror(W64Ctx *c) { return RET_PTR(strerror((int)A(0))); }

/* size_t strxfrm(char *dst, const char *src, size_t n): C locale, so a plain copy. */
static uint64_t m_strxfrm(W64Ctx *c) {
    const char *s = P(1, const char *);
    size_t n = A(2), len = strlen(s);
    char *d = P(0, char *);
    if (d && n) { size_t k = len < n - 1 ? len : n - 1; memcpy(d, s, k); d[k] = 0; }
    return len;
}

/* int sprintf(char *buf, const char *fmt, ...) */
static uint64_t m_sprintf(W64Ctx *c) {
    uint64_t va[VA_MAX], fa[VA_MAX];
    gather_varargs(c, 2, va, fa, VA_MAX);
    char *s = do_format(P(1, const char *), 0, va, fa, VA_MAX);
    if (!s) return (uint32_t)-1;
    size_t n = strlen(s);
    memcpy(P(0, char *), s, n + 1);
    free(s);
    return (uint32_t)n;
}

/* ---- ctype ---- */

static uint64_t m_isspace(W64Ctx *c) { return isspace((int)A(0)) != 0; }

/* ---- conversions ---- */

static uint64_t m_strtoui64(W64Ctx *c) {
    return strtoull(P(0, const char *), P(1, char **), (int)A(2));
}
static uint64_t m_strtoul(W64Ctx *c) {
    return (uint32_t)strtoul(P(0, const char *), P(1, char **), (int)A(2));
}

/* ---- wide strings (16-bit wchar_t) ---- */

static uint64_t m_wcslen(W64Ctx *c) { return wlen16(P(0, const uint16_t *)); }

static int wcmp16(const uint16_t *a, const uint16_t *b, int ci) {
    for (;; a++, b++) {
        uint16_t x = *a, y = *b;
        if (ci) {
            if (x >= 'A' && x <= 'Z') x += 32;
            if (y >= 'A' && y <= 'Z') y += 32;
        }
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

static uint64_t m_wcscmp(W64Ctx *c) { return (uint32_t)wcmp16(P(0, const uint16_t *), P(1, const uint16_t *), 0); }
static uint64_t m_wcsicmp(W64Ctx *c) { return (uint32_t)wcmp16(P(0, const uint16_t *), P(1, const uint16_t *), 1); }

static uint64_t m_wcsxfrm(W64Ctx *c) {
    const uint16_t *s = P(1, const uint16_t *);
    uint16_t *d = P(0, uint16_t *);
    size_t n = A(2), len = wlen16(s);
    if (d && n) { size_t k = len < n - 1 ? len : n - 1; memcpy(d, s, k * 2); d[k] = 0; }
    return len;
}

/* wint_t towlower(wint_t) / towupper */
static uint64_t m_towlower(W64Ctx *c) { return (uint32_t)towlower((wint_t)A(0)); }
static uint64_t m_towupper(W64Ctx *c) { return (uint32_t)towupper((wint_t)A(0)); }

/* int iswctype(wint_t, wctype_t): character classes are not provided, so none match. */
static uint64_t m_iswctype(W64Ctx *c) { (void)c; return 0; }

/* ---- stdio ---- */

static uint64_t m_iob_func(W64Ctx *c) { (void)c; return RET_PTR(iob_area); }

/* FILE *fopen(const char *name, const char *mode) */
static uint64_t m_fopen(W64Ctx *c) {
    char *path = guest_to_host_path(P(0, const char *));
    FILE *f = path ? fopen(path, P(1, const char *)) : NULL;
    free(path);
    return RET_PTR(f);
}
/* FILE *_wfopen(const wchar_t *name, const wchar_t *mode) */
static uint64_t m_wfopen(W64Ctx *c) {
    char *name = utf16_to_utf8(P(0, const uint16_t *));
    char *host = guest_to_host_path(name);
    char *mode = utf16_to_ascii(P(1, const uint16_t *));
    FILE *f = (host && mode) ? fopen(host, mode) : NULL;
    free(host);
    free(name);
    free(mode);
    return RET_PTR(f);
}
/* FILE *_fdopen(int fd, const char *mode) */
static uint64_t m_fdopen(W64Ctx *c) { return RET_PTR(fdopen((int)A(0), P(1, const char *))); }

static uint64_t m_fclose(W64Ctx *c) {
    FILE *f = host_file(P(0, void *));
    if (f == stdin || f == stdout || f == stderr) return 0;
    return (uint32_t)fclose(f);
}
static uint64_t m_fflush(W64Ctx *c) { return (uint32_t)fflush(host_file(P(0, void *))); }
static uint64_t m_fileno(W64Ctx *c) { return (uint32_t)fileno(host_file(P(0, void *))); }

/* int fgetpos(FILE*, fpos_t*): the Windows fpos_t is a 64-bit offset. */
static uint64_t m_fgetpos(W64Ctx *c) {
    off_t pos = ftello(host_file(P(0, void *)));
    if (pos < 0) return (uint32_t)-1;
    memcpy(P(1, void *), &pos, 8);
    return 0;
}
/* int fsetpos(FILE*, const fpos_t*) */
static uint64_t m_fsetpos(W64Ctx *c) {
    int64_t pos;
    memcpy(&pos, P(1, const void *), 8);
    return (uint32_t)fseeko(host_file(P(0, void *)), (off_t)pos, SEEK_SET);
}
/* long ftell(FILE*) */
static uint64_t m_ftell(W64Ctx *c) { return (uint32_t)ftello(host_file(P(0, void *))); }

/* int fprintf(FILE*, const char *fmt, ...) */
static uint64_t m_fprintf(W64Ctx *c) {
    uint64_t va[VA_MAX], fa[VA_MAX];
    gather_varargs(c, 2, va, fa, VA_MAX);
    char *s = do_format(P(1, const char *), 0, va, fa, VA_MAX);
    if (!s) return (uint32_t)-1;
    size_t n = strlen(s);
    fwrite(s, 1, n, host_file(P(0, void *)));
    free(s);
    return (uint32_t)n;
}
/* int fwprintf(FILE*, const wchar_t *fmt, ...) */
static uint64_t m_fwprintf(W64Ctx *c) {
    uint64_t va[VA_MAX], fa[VA_MAX];
    gather_varargs(c, 2, va, fa, VA_MAX);
    char *s = do_format(P(1, const uint16_t *), 1, va, fa, VA_MAX);
    if (!s) return (uint32_t)-1;
    size_t n = strlen(s);
    fwrite(s, 1, n, host_file(P(0, void *)));
    free(s);
    return (uint32_t)n;
}
/* int vfprintf(FILE*, const char *fmt, va_list ap): Win64 va_list is a pointer to 8-byte slots. */
static uint64_t m_vfprintf(W64Ctx *c) {
    const uint64_t *ap = P(2, const uint64_t *);
    char *s = do_format(P(1, const char *), 0, ap, NULL, VA_MAX);
    if (!s) return (uint32_t)-1;
    size_t n = strlen(s);
    fwrite(s, 1, n, host_file(P(0, void *)));
    free(s);
    return (uint32_t)n;
}

/* int fputc(int, FILE*) / putc */
static uint64_t m_fputc(W64Ctx *c) { return (uint32_t)fputc((int)A(0), host_file(P(1, void *))); }
/* int fputwc(wchar_t, FILE*): ASCII only, others become '?'. */
static uint64_t m_fputwc(W64Ctx *c) {
    uint16_t ch = (uint16_t)A(0);
    return (uint32_t)fputc(ch < 0x80 ? ch : '?', host_file(P(1, void *)));
}
/* int fputs(const char *, FILE*) */
static uint64_t m_fputs(W64Ctx *c) { return (uint32_t)fputs(P(0, const char *), host_file(P(1, void *))); }
/* size_t fread(void *buf, size_t size, size_t count, FILE*) */
static uint64_t m_fread(W64Ctx *c) {
    return fread(P(0, void *), A(1), A(2), host_file(P(3, void *)));
}
/* size_t fwrite(const void *buf, size_t size, size_t count, FILE*) */
static uint64_t m_fwrite(W64Ctx *c) {
    return fwrite(P(0, const void *), A(1), A(2), host_file(P(3, void *)));
}
/* int getc(FILE*) / getwc */
static uint64_t m_getc(W64Ctx *c) { return (uint32_t)fgetc(host_file(P(0, void *))); }
/* int ungetc(int, FILE*) / ungetwc */
static uint64_t m_ungetc(W64Ctx *c) { return (uint32_t)ungetc((int)A(0), host_file(P(1, void *))); }

/* int setvbuf(FILE*, char *buf, int mode, size_t size): MSVC modes differ from glibc. */
static uint64_t m_setvbuf(W64Ctx *c) {
    int mode = (int)A(2);
    int gmode = (mode == 0x40) ? _IOLBF : (mode == 4) ? _IONBF : _IOFBF;
    return (uint32_t)setvbuf(host_file(P(0, void *)), P(1, char *), gmode, A(3));
}

/* int _read(int fd, void *buf, unsigned n) / _write */
static uint64_t m_read(W64Ctx *c) { return (uint32_t)read((int)A(0), P(1, void *), (uint32_t)A(2)); }
static uint64_t m_write(W64Ctx *c) { return (uint32_t)write((int)A(0), P(1, const void *), (uint32_t)A(2)); }

/* __int64 _lseeki64(int fd, __int64 off, int whence) */
static uint64_t m_lseeki64(W64Ctx *c) { return (uint64_t)lseek((int)A(0), (off_t)A(1), (int)A(2)); }

/* __int64 _filelengthi64(int fd) */
static uint64_t m_filelengthi64(W64Ctx *c) {
    struct stat st;
    if (fstat((int)A(0), &st) != 0) return (uint64_t)-1;
    return (uint64_t)st.st_size;
}

/* int remove(const char *) */
static uint64_t m_remove(W64Ctx *c) { return (uint32_t)remove(P(0, const char *)); }

/* ---- locale ---- */

/* char *setlocale(int category, const char *locale) */
static uint64_t m_setlocale(W64Ctx *c) { return RET_PTR(setlocale((int)A(0), P(1, const char *))); }
/* struct lconv *localeconv(void): glibc and MSVC share the leading char* fields. */
static uint64_t m_localeconv(W64Ctx *c) { (void)c; return RET_PTR(localeconv()); }
/* UINT ___lc_codepage_func(void): 0 means the ANSI code page, treated as UTF-8 here. */
static uint64_t m_lc_codepage(W64Ctx *c) { (void)c; return 0; }
/* int ___mb_cur_max_func(void): one byte per character in the C locale. */
static uint64_t m_mb_cur_max(W64Ctx *c) { (void)c; return 1; }

/* size_t strftime(char *, size_t, const char *, const struct tm *)
 * Guest struct tm is nine ints (36 bytes). glibc's is larger, so copy the fields. */
static uint64_t m_strftime(W64Ctx *c) {
    const int *g = P(3, const int *);
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_sec = g[0]; t.tm_min = g[1]; t.tm_hour = g[2]; t.tm_mday = g[3];
    t.tm_mon = g[4]; t.tm_year = g[5]; t.tm_wday = g[6]; t.tm_yday = g[7]; t.tm_isdst = g[8];
    return strftime(P(0, char *), A(1), P(2, const char *), &t);
}
/* size_t wcsftime(...): wide date formatting is not provided. */
static uint64_t m_wcsftime(W64Ctx *c) { (void)c; return 0; }

/* ---- process, errors, runtime ---- */

/* char *getenv(const char *) */
static uint64_t m_getenv(W64Ctx *c) { return RET_PTR(getenv(P(0, const char *))); }

/* int *_errno(void) */
static uint64_t m_errno(W64Ctx *c) { (void)c; return RET_PTR(__errno_location()); }

/* void abort(void) */
static uint64_t m_abort(W64Ctx *c) { (void)c; abort(); }

/* void _amsg_exit(int) */
static uint64_t m_amsg_exit(W64Ctx *c) { exit((int)A(0)); }

/* void _assert(const char *expr, const char *file, unsigned line) */
static uint64_t m_assert(W64Ctx *c) {
    fprintf(stderr, "Assertion failed: %s, file %s, line %u\n",
            P(0, const char *), P(1, const char *), (unsigned)A(2));
    abort();
}

/* _onexit-style pointer setter: not needed, return 0. */
static uint64_t m_setusermatherr(W64Ctx *c) { (void)c; return 0; }

/* ---- startup tables ----
 * _initterm(first, last) runs each non-NULL function pointer in [first, last), Win64 ABI. */
static uint64_t m_initterm(W64Ctx *c) {
    void (**first)(void) = P(0, void (**)(void));
    void (**last)(void) = P(1, void (**)(void));
    for (void (**p)(void) = first; p < last; p++)
        if (*p) xbemu_call_w64((void *)*p, 0, 0, 0, 0);
    return 0;
}

/* _lock(int) / _unlock(int): per-index recursive locks, as in MSVC's _LOCKTAB. */
#define LOCK_COUNT 64
static pthread_mutex_t crt_locks[LOCK_COUNT] = { [0 ... LOCK_COUNT - 1] = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP };

static uint64_t m_lock(W64Ctx *c) {
    unsigned i = (unsigned)A(0) % LOCK_COUNT;
    pthread_mutex_lock(&crt_locks[i]);
    return 0;
}
static uint64_t m_unlock(W64Ctx *c) {
    unsigned i = (unsigned)A(0) % LOCK_COUNT;
    pthread_mutex_unlock(&crt_locks[i]);
    return 0;
}

/* double ldexp(double x, int e): x in xmm0, e in the second integer slot. */
static uint64_t m_ldexp(W64Ctx *c) {
    w64_ret_f64(c, ldexp(w64_arg_f64(c, 0), (int)(uint32_t)A(1)));
    return 0;
}

/* ---- process startup and exit ----
 * The C runtime's startup code (ordinary programs, not freestanding ones) calls these. */

static int g_argc;
static char **g_argv;
static char **g_envp;

void hostlib_set_args(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
    g_envp = environ;
}

static int fmode_v = 0;     /* _fmode: default text/binary mode */
static int commode_v = 0;   /* _commode: default commit mode */
static char **initenv_v;    /* __initenv: environment block */

HostData msvcrt_data[] = {
    { "_fmode",    &fmode_v   },
    { "_commode",  &commode_v },
    { "__initenv", &initenv_v },
    { NULL, NULL },
};

/* int __getmainargs(int *argc, char ***argv, char ***envp, int wildcard, void *startinfo) */
static uint64_t m_getmainargs(W64Ctx *c) {
    int *argc = P(0, int *);
    char ***argv = P(1, char ***);
    char ***envp = P(2, char ***);
    if (argc) *argc = g_argc;
    if (argv) *argv = g_argv;
    if (envp) *envp = g_envp;
    initenv_v = g_envp;
    return 0;
}

#define ATEXIT_MAX 64
static void (*atexit_fns[ATEXIT_MAX])(void);
static int atexit_count;

/* int atexit(void (*fn)(void)) */
static uint64_t m_atexit(W64Ctx *c) {
    if (atexit_count >= ATEXIT_MAX) return (uint32_t)-1;
    atexit_fns[atexit_count++] = P(0, void (*)(void));
    return 0;
}

/* Run registered handlers, last registered first, then exit. Does not return. */
static void run_exit(int code) {
    while (atexit_count > 0) {
        void (*fn)(void) = atexit_fns[--atexit_count];
        if (fn) xbemu_call_w64((void *)fn, 0, 0, 0, 0);
    }
    fflush(NULL);
    exit(code);
}

/* void exit(int) and _cexit(void): both run atexit handlers here. */
static uint64_t m_exit(W64Ctx *c) { run_exit((int)A(0)); return 0; }
/* void _exit(int): no handlers. */
static uint64_t m_underscore_exit(W64Ctx *c) { fflush(NULL); _exit((int)A(0)); return 0; }

/* void __set_app_type(int): console or GUI. Nothing to set. */
static uint64_t m_set_app_type(W64Ctx *c) { (void)c; return 0; }

/* sighandler_t signal(int sig, sighandler_t handler): handlers are not delivered. */
static uint64_t m_signal(W64Ctx *c) { (void)c; return 0; }

/* EXCEPTION_DISPOSITION __C_specific_handler(...): no __try blocks, so keep searching. */
static uint64_t m_c_specific_handler(W64Ctx *c) { (void)c; return 1; }

/* ---- stdio and formatted output ---- */

static uint64_t m_printf(W64Ctx *c) {
    uint64_t va[VA_MAX], fa[VA_MAX];
    gather_varargs(c, 1, va, fa, VA_MAX);
    char *s = do_format(P(0, const char *), 0, va, fa, VA_MAX);
    if (!s) return (uint32_t)-1;
    size_t n = strlen(s);
    fwrite(s, 1, n, stdout);
    free(s);
    return (uint32_t)n;
}

static uint64_t m_puts(W64Ctx *c) {
    int r = fputs(P(0, const char *), stdout);
    if (r >= 0) fputc('\n', stdout);
    return (uint32_t)(r >= 0 ? 0 : -1);
}

static uint64_t m_putchar(W64Ctx *c) { return (uint32_t)fputc((int)A(0), stdout); }

/* int snprintf(char *buf, size_t n, const char *fmt, ...): returns the length that would be written. */
static int copy_bounded(char *buf, size_t n, const char *s) {
    int len = (int)strlen(s);
    if (buf && n) {
        size_t k = (size_t)len < n - 1 ? (size_t)len : n - 1;
        memcpy(buf, s, k);
        buf[k] = 0;
    }
    return len;
}

static uint64_t m_snprintf(W64Ctx *c) {
    uint64_t va[VA_MAX], fa[VA_MAX];
    gather_varargs(c, 3, va, fa, VA_MAX);
    char *s = do_format(P(2, const char *), 0, va, fa, VA_MAX);
    if (!s) return (uint32_t)-1;
    int r = copy_bounded(P(0, char *), A(1), s);
    free(s);
    return (uint32_t)r;
}

static uint64_t m_vsnprintf(W64Ctx *c) {
    char *s = do_format(P(2, const char *), 0, P(3, const uint64_t *), NULL, VA_MAX);
    if (!s) return (uint32_t)-1;
    int r = copy_bounded(P(0, char *), A(1), s);
    free(s);
    return (uint32_t)r;
}

static uint64_t m_vsprintf(W64Ctx *c) {
    char *s = do_format(P(1, const char *), 0, P(2, const uint64_t *), NULL, VA_MAX);
    if (!s) return (uint32_t)-1;
    size_t n = strlen(s);
    memcpy(P(0, char *), s, n + 1);
    free(s);
    return (uint32_t)n;
}

static uint64_t m_fseek(W64Ctx *c) {
    return (uint32_t)fseeko(host_file(P(0, void *)), (off_t)(int32_t)A(1), (int)A(2));
}
static uint64_t m_feof(W64Ctx *c) { return (uint32_t)feof(host_file(P(0, void *))); }
static uint64_t m_ferror(W64Ctx *c) { return (uint32_t)ferror(host_file(P(0, void *))); }
static uint64_t m_fgets(W64Ctx *c) {
    return RET_PTR(fgets(P(0, char *), (int)A(1), host_file(P(2, void *))));
}
static uint64_t m_getchar(W64Ctx *c) { (void)c; return (uint32_t)getchar(); }

/* ---- strings and characters ---- */

static uint64_t m_strcpy(W64Ctx *c) { return RET_PTR(strcpy(P(0, char *), P(1, const char *))); }
static uint64_t m_strcat(W64Ctx *c) { return RET_PTR(strcat(P(0, char *), P(1, const char *))); }
static uint64_t m_strrchr(W64Ctx *c) { return RET_PTR(strrchr(P(0, const char *), (int)A(1))); }
static uint64_t m_strtol(W64Ctx *c) {
    return (uint32_t)strtol(P(0, const char *), P(1, char **), (int)A(2));
}
static uint64_t m_atoi(W64Ctx *c) { return (uint32_t)atoi(P(0, const char *)); }
static uint64_t m_atol(W64Ctx *c) { return (uint32_t)(long)atol(P(0, const char *)); }
static uint64_t m_stricmp(W64Ctx *c) { return (uint32_t)strcasecmp(P(0, const char *), P(1, const char *)); }
static uint64_t m_strnicmp(W64Ctx *c) { return (uint32_t)strncasecmp(P(0, const char *), P(1, const char *), A(2)); }

static uint64_t m_isalpha(W64Ctx *c) { return isalpha((int)A(0)) != 0; }
static uint64_t m_isdigit(W64Ctx *c) { return isdigit((int)A(0)) != 0; }
static uint64_t m_isalnum(W64Ctx *c) { return isalnum((int)A(0)) != 0; }
static uint64_t m_isupper(W64Ctx *c) { return isupper((int)A(0)) != 0; }
static uint64_t m_islower(W64Ctx *c) { return islower((int)A(0)) != 0; }
static uint64_t m_tolower(W64Ctx *c) { return (uint32_t)tolower((int)A(0)); }
static uint64_t m_toupper(W64Ctx *c) { return (uint32_t)toupper((int)A(0)); }

/* ---- numbers, time, sorting ---- */

static uint64_t m_rand(W64Ctx *c) { (void)c; return (uint32_t)rand(); }
static uint64_t m_srand(W64Ctx *c) { srand((unsigned)A(0)); return 0; }
static uint64_t m_time(W64Ctx *c) {
    time_t t = time(NULL);
    if (P(0, time_t *)) *P(0, time_t *) = t;
    return (uint64_t)t;
}
static uint64_t m_clock(W64Ctx *c) { (void)c; return (uint32_t)clock(); }

/* qsort calls a guest comparator. The comparator is kept here for the host callback. */
static void *qsort_cmp_guest;
static int qsort_cmp_host(const void *a, const void *b) {
    return (int)(int32_t)xbemu_call_w64(qsort_cmp_guest, (uint64_t)(uintptr_t)a,
                                        (uint64_t)(uintptr_t)b, 0, 0);
}
/* void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) */
static uint64_t m_qsort(W64Ctx *c) {
    qsort_cmp_guest = P(3, void *);
    qsort(P(0, void *), A(1), A(2), qsort_cmp_host);
    return 0;
}

/* ---- export table ---- */

const HostExport msvcrt_exports[] = {
    { "malloc",              m_malloc              },
    { "calloc",              m_calloc              },
    { "realloc",             m_realloc             },
    { "free",                m_free                },
    { "_aligned_malloc",     m_aligned_malloc     },
    { "_aligned_free",       m_aligned_free       },
    { "memcpy",              m_memcpy              },
    { "memmove",             m_memmove             },
    { "memset",              m_memset              },
    { "memcmp",              m_memcmp              },
    { "memchr",              m_memchr              },
    { "strlen",              m_strlen              },
    { "strcmp",              m_strcmp              },
    { "strncmp",             m_strncmp             },
    { "strchr",              m_strchr              },
    { "strstr",              m_strstr              },
    { "strncpy",             m_strncpy             },
    { "strcoll",             m_strcoll             },
    { "strerror",            m_strerror            },
    { "strxfrm",             m_strxfrm             },
    { "sprintf",             m_sprintf             },
    { "isspace",             m_isspace             },
    { "_strtoui64",          m_strtoui64           },
    { "strtoul",             m_strtoul             },
    { "wcslen",              m_wcslen              },
    { "wcscmp",              m_wcscmp              },
    { "wcscoll",             m_wcscmp              },
    { "_wcsicmp",            m_wcsicmp             },
    { "wcsxfrm",             m_wcsxfrm             },
    { "towlower",            m_towlower            },
    { "towupper",            m_towupper            },
    { "iswctype",            m_iswctype            },
    { "__iob_func",          m_iob_func            },
    { "fopen",               m_fopen               },
    { "_wfopen",             m_wfopen              },
    { "_fdopen",             m_fdopen              },
    { "fclose",              m_fclose              },
    { "fflush",              m_fflush              },
    { "_fileno",             m_fileno              },
    { "fgetpos",             m_fgetpos             },
    { "fsetpos",             m_fsetpos             },
    { "ftell",               m_ftell               },
    { "fprintf",             m_fprintf             },
    { "fwprintf",            m_fwprintf            },
    { "vfprintf",            m_vfprintf            },
    { "fputc",               m_fputc               },
    { "putc",                m_fputc               },
    { "fputwc",              m_fputwc              },
    { "putwc",               m_fputwc              },
    { "fputs",               m_fputs               },
    { "fread",               m_fread               },
    { "fwrite",              m_fwrite              },
    { "getc",                m_getc                },
    { "getwc",               m_getc                },
    { "ungetc",              m_ungetc              },
    { "ungetwc",             m_ungetc              },
    { "setvbuf",             m_setvbuf             },
    { "_read",               m_read                },
    { "_write",              m_write               },
    { "_lseeki64",           m_lseeki64            },
    { "_filelengthi64",      m_filelengthi64       },
    { "remove",              m_remove              },
    { "setlocale",           m_setlocale           },
    { "localeconv",          m_localeconv          },
    { "___lc_codepage_func", m_lc_codepage         },
    { "___mb_cur_max_func",  m_mb_cur_max          },
    { "strftime",            m_strftime            },
    { "wcsftime",            m_wcsftime            },
    { "getenv",              m_getenv              },
    { "_errno",              m_errno               },
    { "abort",               m_abort               },
    { "_amsg_exit",          m_amsg_exit           },
    { "_assert",             m_assert              },
    { "__setusermatherr",    m_setusermatherr      },
    { "_initterm",           m_initterm            },
    { "_lock",               m_lock                },
    { "_unlock",             m_unlock              },
    { "ldexp",               m_ldexp               },
    { "__getmainargs",       m_getmainargs         },
    { "atexit",              m_atexit              },
    { "exit",                m_exit                },
    { "_cexit",              m_exit                },
    { "_exit",               m_underscore_exit     },
    { "__set_app_type",      m_set_app_type        },
    { "signal",              m_signal              },
    { "__C_specific_handler", m_c_specific_handler },
    { "printf",              m_printf              },
    { "puts",                m_puts                },
    { "putchar",             m_putchar             },
    { "snprintf",            m_snprintf            },
    { "_snprintf",           m_snprintf            },
    { "_vsnprintf",          m_vsnprintf           },
    { "vsprintf",            m_vsprintf            },
    { "fseek",               m_fseek               },
    { "feof",                m_feof                },
    { "ferror",              m_ferror              },
    { "fgets",               m_fgets               },
    { "getchar",             m_getchar             },
    { "strcpy",              m_strcpy              },
    { "strcat",              m_strcat              },
    { "strrchr",             m_strrchr             },
    { "strtol",              m_strtol              },
    { "atoi",                m_atoi                },
    { "atol",                m_atol                },
    { "_stricmp",            m_stricmp             },
    { "_strnicmp",           m_strnicmp            },
    { "isalpha",             m_isalpha             },
    { "isdigit",             m_isdigit             },
    { "isalnum",             m_isalnum             },
    { "isupper",             m_isupper             },
    { "islower",             m_islower             },
    { "tolower",             m_tolower             },
    { "toupper",             m_toupper             },
    { "rand",                m_rand                },
    { "srand",               m_srand               },
    { "time",                m_time                },
    { "clock",               m_clock               },
    { "qsort",               m_qsort               },
    { NULL, NULL },
};

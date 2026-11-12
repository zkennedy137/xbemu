/*
 * advapi32.dll shims: locally unique IDs and the registry.
 *
 * The registry is an in-memory tree of keys and values, with the standard root keys
 * (HKCR, HKCU, HKLM, HKU). If XBEMU_REGISTRY names a file, it is loaded at first use and
 * rewritten on every change, so settings persist between runs. Without it, the registry
 * lasts for one process.
 *
 * File format, one record per line, with tab-separated fields:
 *   K <key path>
 *   V <key path> <value name> <type> <hex data>
 * Strings are stored as UTF-8 and given back as ANSI or UTF-16 as the caller asks.
 */
#define _DEFAULT_SOURCE
#include "kernel32_impl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_MORE_DATA 234u
#define ERROR_BAD_PATHNAME 161u
#define REG_NONE 0u
#define REG_SZ 1u
#define REG_BINARY 3u
#define REG_DWORD 4u

/* Predefined root handles (values from winreg.h). */
#define HKEY_CLASSES_ROOT  ((uintptr_t)0x80000000u)
#define HKEY_CURRENT_USER  ((uintptr_t)0x80000001u)
#define HKEY_LOCAL_MACHINE ((uintptr_t)0x80000002u)
#define HKEY_USERS         ((uintptr_t)0x80000003u)

typedef struct RegValue {
    char *name;      /* UTF-8, case-insensitive match */
    uint32_t type;
    uint8_t *data;
    uint32_t size;
    struct RegValue *next;
} RegValue;

typedef struct RegKey {
    char *path;      /* canonical: "ROOT\\sub\\key", root names upper case */
    RegValue *values;
    struct RegKey *next;
} RegKey;

/* An open-key handle is a pointer to one of these. */
typedef struct RegHandle {
    uintptr_t magic;     /* REG_HANDLE_MAGIC: lets root handles be told apart */
    RegKey *key;
} RegHandle;
#define REG_HANDLE_MAGIC 0x52454748u

static RegKey *keys;
static int loaded;
static const char *store_path;

/* ---- path helpers ---- */

static char *utf16_to_utf8_dup(const uint16_t *s) {
    if (!s) return NULL;
    size_t n = 0;
    while (s[n]) n++;
    char *out = malloc(n * 4 + 1), *o = out;
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) *o++ = (char)cp;
        else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
            *o++ = (char)(0x80 | (cp & 0x3F));
        } else {
            *o++ = (char)(0xF0 | (cp >> 18)); *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
            *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F));
        }
    }
    *o = 0;
    return out;
}

/* Subkey path: the root (a predefined handle or an open key) plus a relative path. */
static char *make_path(RegKey *base, const char *base_root, const char *sub) {
    char *p;
    if (base) {
        size_t n = strlen(base->path) + strlen(sub) + 2;
        p = malloc(n);
        if (!p) return NULL;
        snprintf(p, n, "%s\\%s", base->path, sub);
    } else {
        size_t n = strlen(base_root) + strlen(sub) + 2;
        p = malloc(n);
        if (!p) return NULL;
        snprintf(p, n, "%s%s%s", base_root, *sub ? "\\" : "", sub);
    }
    /* Normalize: forward slashes become backslashes, and trailing backslashes are removed. */
    for (char *q = p; *q; q++) if (*q == '/') *q = '\\';
    size_t len = strlen(p);
    while (len > 0 && p[len - 1] == '\\') p[--len] = 0;
    return p;
}

/* Predefined HKEYs are 32-bit values that arrive sign-extended to 64 bits. */
static uintptr_t norm_hkey(uintptr_t h) {
    int64_t v = (int64_t)h;
    if (v < 0 && v >= -0x80000000LL) return h & 0xFFFFFFFFu;
    return h;
}

static const char *root_name(uintptr_t h) {
    switch (norm_hkey(h)) {
    case HKEY_CLASSES_ROOT:  return "HKCR";
    case HKEY_CURRENT_USER:  return "HKCU";
    case HKEY_LOCAL_MACHINE: return "HKLM";
    case HKEY_USERS:         return "HKU";
    default: return NULL;
    }
}

static RegKey *find_key(const char *path) {
    for (RegKey *k = keys; k; k = k->next)
        if (strcasecmp(k->path, path) == 0) return k;
    return NULL;
}

static RegKey *ensure_key(const char *path) {
    RegKey *k = find_key(path);
    if (k) return k;
    k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->path = strdup(path);
    k->next = keys;
    keys = k;
    return k;
}

/* ---- persistence ---- */

static void hex_encode(FILE *f, const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) fprintf(f, "%02x", p[i]);
}

static int hex_decode(const char *s, uint8_t *out, uint32_t cap, uint32_t *n) {
    uint32_t k = 0;
    for (; s[0] && s[1]; s += 2) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) return 0;
        if (k >= cap) return 0;
        out[k++] = (uint8_t)v;
    }
    *n = k;
    return 1;
}

static void save_store(void) {
    if (!store_path) return;
    FILE *f = fopen(store_path, "w");
    if (!f) return;
    for (RegKey *k = keys; k; k = k->next) {
        fprintf(f, "K %s\n", k->path);
        for (RegValue *v = k->values; v; v = v->next) {
            fprintf(f, "V %s\t%s\t%u\t", k->path, v->name, v->type);
            hex_encode(f, v->data, v->size);
            fprintf(f, "\n");
        }
    }
    fclose(f);
}

static void load_store(void) {
    if (loaded) return;
    loaded = 1;
    store_path = getenv("XBEMU_REGISTRY");
    if (!store_path || !*store_path) return;
    FILE *f = fopen(store_path, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (line[0] == 'K' && line[1] == ' ') {
            ensure_key(line + 2);
        } else if (line[0] == 'V' && line[1] == ' ') {
            char *path = line + 2, *name = strchr(path, '\t');
            if (!name) continue;
            *name++ = 0;
            char *type = strchr(name, '\t');
            if (!type) continue;
            *type++ = 0;
            char *hex = strchr(type, '\t');
            if (!hex) continue;
            *hex++ = 0;
            RegKey *k = ensure_key(path);
            if (!k) continue;
            RegValue *v = calloc(1, sizeof *v);
            if (!v) continue;
            v->name = strdup(name);
            v->type = (uint32_t)strtoul(type, NULL, 10);
            v->data = malloc(strlen(hex) / 2 + 1);
            if (!v->data || !hex_decode(hex, v->data, (uint32_t)(strlen(hex) / 2 + 1), &v->size)) {
                free(v->name); free(v->data); free(v);
                continue;
            }
            v->next = k->values;
            k->values = v;
        }
    }
    fclose(f);
}

/* ---- handles ---- */

static uintptr_t new_handle(RegKey *k) {
    RegHandle *h = calloc(1, sizeof *h);
    if (!h) return 0;
    h->magic = REG_HANDLE_MAGIC;
    h->key = k;
    return (uintptr_t)h;
}

/* Resolve an HKEY (predefined root or open handle) to the key path it names. */
static int resolve_base(uintptr_t hkey, RegKey **key, const char **root) {
    hkey = norm_hkey(hkey);
    *key = NULL;
    *root = root_name(hkey);
    if (*root) return 1;
    if (!hkey) return 0;
    RegHandle *h = (RegHandle *)hkey;
    if (h->magic != REG_HANDLE_MAGIC) return 0;
    *key = h->key;
    return 1;
}

/* Common open/create body. sub is UTF-8. */
static uint32_t open_key(uintptr_t hkey, const char *sub, uintptr_t *out, int create) {
    load_store();
    RegKey *base;
    const char *root;
    if (!resolve_base(hkey, &base, &root)) return ERROR_BAD_PATHNAME;
    char *path = make_path(base, root, sub ? sub : "");
    if (!path) return ERROR_FILE_NOT_FOUND;
    RegKey *k = create ? ensure_key(path) : find_key(path);
    free(path);
    if (!k) return ERROR_FILE_NOT_FOUND;
    *out = new_handle(k);
    if (create) save_store();
    return *out ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

static RegValue *find_value(RegKey *k, const char *name) {
    for (RegValue *v = k->values; v; v = v->next)
        if (strcasecmp(v->name, name ? name : "") == 0) return v;
    return NULL;
}

static RegKey *key_of(uintptr_t hkey) {
    RegKey *base;
    const char *root;
    if (!resolve_base(hkey, &base, &root)) return NULL;
    if (root) {
        /* Root handles hold values too, under the root's own key node. */
        char *path = make_path(NULL, root, "");
        RegKey *k = ensure_key(path);
        free(path);
        return k;
    }
    return base;
}

/* Copy a value out, converting strings to ANSI or UTF-16 as the caller asks. */
static uint32_t query_value(uintptr_t hkey, const char *name, int want_wide, uint32_t *type,
                            void *data, uint32_t *cbdata) {
    load_store();
    RegKey *k = key_of(hkey);
    if (!k) return ERROR_FILE_NOT_FOUND;
    RegValue *v = find_value(k, name);
    if (!v) return ERROR_FILE_NOT_FOUND;

    uint8_t *src = v->data;
    uint32_t n = v->size;
    uint8_t *conv = NULL;
    if (v->type == REG_SZ && want_wide) {
        /* UTF-8 to UTF-16 with a terminator. */
        size_t cap = (size_t)n * 2 + 2;
        conv = malloc(cap);
        if (!conv) return ERROR_FILE_NOT_FOUND;
        uint16_t *w = (uint16_t *)conv;
        size_t i = 0;
        for (uint32_t b = 0; b < n && v->data[b]; b++) w[i++] = (uint16_t)(unsigned char)v->data[b];
        w[i] = 0;
        n = (uint32_t)(i * 2 + 2);
        src = conv;
    } else if (v->type == REG_SZ) {
        n = (uint32_t)strlen((const char *)v->data) + 1;
    }

    if (type) *type = v->type;
    uint32_t result = ERROR_SUCCESS;
    if (data && cbdata && *cbdata < n) {
        result = ERROR_MORE_DATA;
    } else if (data) {
        memcpy(data, src, n);
    }
    if (cbdata) *cbdata = n;
    free(conv);
    return result;
}

static uint32_t set_value(uintptr_t hkey, const char *name, uint32_t type, const void *data, uint32_t n) {
    load_store();
    RegKey *k = key_of(hkey);
    if (!k) return ERROR_FILE_NOT_FOUND;
    RegValue *v = find_value(k, name);
    if (!v) {
        v = calloc(1, sizeof *v);
        if (!v) return ERROR_FILE_NOT_FOUND;
        v->name = strdup(name ? name : "");
        v->next = k->values;
        k->values = v;
    }
    free(v->data);
    v->data = malloc(n ? n : 1);
    if (!v->data) return ERROR_FILE_NOT_FOUND;
    memcpy(v->data, data, n);
    v->size = n;
    v->type = type;
    save_store();
    return ERROR_SUCCESS;
}

/* ---- exports ---- */

/* BOOL AllocateLocallyUniqueId(PLUID): LUID is { DWORD LowPart; LONG HighPart; }. */
static uint64_t adv_AllocateLocallyUniqueId(W64Ctx *c) {
    static uint64_t next = 0x3e7 + 1; /* values above SYSTEM_LUID (0x3e7) */
    uint32_t *luid = (uint32_t *)(uintptr_t)w64_arg(c, 0);
    if (!luid) return 0;
    uint64_t v = __atomic_add_fetch(&next, 1, __ATOMIC_SEQ_CST);
    luid[0] = (uint32_t)v;
    luid[1] = (uint32_t)(v >> 32);
    return 1;
}

/* LSTATUS RegOpenKeyExA(HKEY, LPCSTR sub, DWORD options, REGSAM, PHKEY) */
static uint64_t adv_RegOpenKeyExA(W64Ctx *c) {
    uintptr_t *out = (uintptr_t *)(uintptr_t)w64_arg(c, 4);
    if (!out) return ERROR_BAD_PATHNAME;
    return open_key((uintptr_t)w64_arg(c, 0), (const char *)(uintptr_t)w64_arg(c, 1), out, 0);
}

/* LSTATUS RegOpenKeyExW(HKEY, LPCWSTR sub, DWORD options, REGSAM, PHKEY) */
static uint64_t adv_RegOpenKeyExW(W64Ctx *c) {
    uintptr_t *out = (uintptr_t *)(uintptr_t)w64_arg(c, 4);
    if (!out) return ERROR_BAD_PATHNAME;
    char *sub = utf16_to_utf8_dup((const uint16_t *)(uintptr_t)w64_arg(c, 1));
    uint32_t r = open_key((uintptr_t)w64_arg(c, 0), sub ? sub : "", out, 0);
    free(sub);
    return r;
}

/* LSTATUS RegCreateKeyExA(HKEY, LPCSTR, DWORD, LPSTR class, DWORD opts, REGSAM, LPSECURITY, PHKEY, LPDWORD disp) */
static uint64_t adv_RegCreateKeyExA(W64Ctx *c) {
    uintptr_t *out = (uintptr_t *)(uintptr_t)w64_arg(c, 7);
    if (!out) return ERROR_BAD_PATHNAME;
    return open_key((uintptr_t)w64_arg(c, 0), (const char *)(uintptr_t)w64_arg(c, 1), out, 1);
}

/* LSTATUS RegQueryValueExA(HKEY, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD cbData) */
static uint64_t adv_RegQueryValueExA(W64Ctx *c) {
    uint32_t *type = (uint32_t *)(uintptr_t)w64_arg(c, 3);
    void *data = (void *)(uintptr_t)w64_arg(c, 4);
    uint32_t *cb = (uint32_t *)(uintptr_t)w64_arg(c, 5);
    return query_value((uintptr_t)w64_arg(c, 0), (const char *)(uintptr_t)w64_arg(c, 1), 0, type, data, cb);
}

/* LSTATUS RegQueryValueExW(HKEY, LPCWSTR name, ...) */
static uint64_t adv_RegQueryValueExW(W64Ctx *c) {
    uint32_t *type = (uint32_t *)(uintptr_t)w64_arg(c, 3);
    void *data = (void *)(uintptr_t)w64_arg(c, 4);
    uint32_t *cb = (uint32_t *)(uintptr_t)w64_arg(c, 5);
    char *name = utf16_to_utf8_dup((const uint16_t *)(uintptr_t)w64_arg(c, 1));
    uint32_t r = query_value((uintptr_t)w64_arg(c, 0), name ? name : "", 1, type, data, cb);
    free(name);
    return r;
}

/* LSTATUS RegSetValueExA(HKEY, LPCSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD cb) */
static uint64_t adv_RegSetValueExA(W64Ctx *c) {
    const uint8_t *data = (const uint8_t *)(uintptr_t)w64_arg(c, 4);
    uint32_t cb = (uint32_t)w64_arg(c, 5);
    return set_value((uintptr_t)w64_arg(c, 0), (const char *)(uintptr_t)w64_arg(c, 1),
                     (uint32_t)w64_arg(c, 3), data, cb);
}

/* LSTATUS RegSetValueExW: wide string data is stored as UTF-8. */
static uint64_t adv_RegSetValueExW(W64Ctx *c) {
    char *name = utf16_to_utf8_dup((const uint16_t *)(uintptr_t)w64_arg(c, 1));
    uint32_t type = (uint32_t)w64_arg(c, 3);
    const uint8_t *data = (const uint8_t *)(uintptr_t)w64_arg(c, 4);
    uint32_t cb = (uint32_t)w64_arg(c, 5);
    uint32_t r;
    if (type == REG_SZ && data) {
        char *utf8 = utf16_to_utf8_dup((const uint16_t *)data);
        r = set_value((uintptr_t)w64_arg(c, 0), name ? name : "", REG_SZ, utf8 ? utf8 : "",
                      utf8 ? (uint32_t)strlen(utf8) + 1 : 1);
        free(utf8);
    } else {
        r = set_value((uintptr_t)w64_arg(c, 0), name ? name : "", type, data, cb);
    }
    free(name);
    return r;
}

/* LSTATUS RegCloseKey(HKEY): frees handles from RegOpenKeyEx/RegCreateKeyEx; roots are no-ops. */
static uint64_t adv_RegCloseKey(W64Ctx *c) {
    uintptr_t h = norm_hkey((uintptr_t)w64_arg(c, 0));
    if (!h || root_name(h)) return ERROR_SUCCESS;
    RegHandle *rh = (RegHandle *)h;
    if (rh->magic == REG_HANDLE_MAGIC) {
        rh->magic = 0;
        free(rh);
    }
    return ERROR_SUCCESS;
}

/* LSTATUS RegNotifyChangeKeyValue(...): change notifications are not delivered. */
static uint64_t adv_RegNotifyChangeKeyValue(W64Ctx *c) {
    (void)c;
    return ERROR_FILE_NOT_FOUND;
}

const HostExport advapi32_exports[] = {
    { "AllocateLocallyUniqueId",     adv_AllocateLocallyUniqueId },
    { "RegOpenKeyExA",               adv_RegOpenKeyExA           },
    { "RegOpenKeyExW",               adv_RegOpenKeyExW           },
    { "RegCreateKeyExA",             adv_RegCreateKeyExA         },
    { "RegQueryValueExA",            adv_RegQueryValueExA        },
    { "RegQueryValueExW",            adv_RegQueryValueExW        },
    { "RegSetValueExA",              adv_RegSetValueExA          },
    { "RegSetValueExW",              adv_RegSetValueExW          },
    { "RegCloseKey",                 adv_RegCloseKey             },
    { "RegNotifyChangeKeyValue",     adv_RegNotifyChangeKeyValue },
    { NULL, NULL },
};

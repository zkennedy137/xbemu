/*
 * kernel32 file and console I/O: CreateFile, ReadFile, WriteFile, directories,
 * attributes, and thread priority.
 *
 * Paths are guest (Windows-style) paths, translated by guest_to_host_path.
 * Standard handles are fixed small values: 1 stdout, 2 stderr, 3 stdin.
 */
#define _DEFAULT_SOURCE
#include "kernel32_impl.h"
#include "paths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HANDLE_STDOUT 1
#define HANDLE_STDERR 2
#define HANDLE_STDIN  3
#define INVALID_HANDLE_VALUE ((uint64_t)-1)

#define GENERIC_READ   0x80000000u
#define GENERIC_WRITE  0x40000000u
#define CREATE_NEW        1u
#define CREATE_ALWAYS     2u
#define OPEN_EXISTING     3u
#define OPEN_ALWAYS       4u
#define TRUNCATE_EXISTING 5u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL    0x80u
#define INVALID_FILE_ATTRIBUTES  0xFFFFFFFFu

/* UTF-16 to UTF-8 for host paths. */
static char *wide_to_utf8(const uint16_t *s) {
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

/* Host path for a guest path given as UTF-16 or ASCII. Caller frees. */
static char *host_path_w(const uint16_t *wname) {
    char *u = wide_to_utf8(wname);
    if (!u) return NULL;
    char *h = guest_to_host_path(u);
    free(u);
    return h;
}

static uint32_t errno_to_win(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR: return WIN_ERROR_FILE_NOT_FOUND;
    case EEXIST: return WIN_ERROR_FILE_EXISTS;
    case EACCES: case EPERM: return WIN_ERROR_ACCESS_DENIED;
    default: return (uint32_t)e;
    }
}

/* Shared body of CreateFileA/W. */
static uint64_t create_file(const char *host, uint32_t access, uint32_t creation) {
    int flags = 0;
    int read = (access & GENERIC_READ) != 0, write = (access & GENERIC_WRITE) != 0;
    flags |= (read && write) ? O_RDWR : write ? O_WRONLY : O_RDONLY;
    switch (creation) {
    case CREATE_NEW:        flags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS:     flags |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS:       flags |= O_CREAT; break;
    case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    default: break; /* OPEN_EXISTING */
    }
    int fd = open(host, flags, 0660);
    if (fd < 0) { k32_set_last_error(errno_to_win(errno)); return INVALID_HANDLE_VALUE; }
    /* CREATE_ALWAYS and OPEN_ALWAYS on an existing file report ERROR_ALREADY_EXISTS. */
    if ((creation == CREATE_ALWAYS || creation == OPEN_ALWAYS) && errno == 0) k32_set_last_error(0);

    Obj *o = k32_obj_new(OBJ_FILE);
    if (!o) { close(fd); k32_set_last_error(8); return INVALID_HANDLE_VALUE; }
    o->fd = fd;
    return (uint64_t)(uintptr_t)o;
}

/* HANDLE CreateFileW(LPCWSTR, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES, DWORD creation,
 *                    DWORD flags, HANDLE template) */
static uint64_t f_CreateFileW(W64Ctx *c) {
    char *host = host_path_w((const uint16_t *)(uintptr_t)w64_arg(c, 0));
    if (!host) { k32_set_last_error(WIN_ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    uint64_t h = create_file(host, (uint32_t)w64_arg(c, 1), (uint32_t)w64_arg(c, 4));
    free(host);
    return h;
}

/* HANDLE CreateFileA(LPCSTR, ...) */
static uint64_t f_CreateFileA(W64Ctx *c) {
    char *host = guest_to_host_path((const char *)(uintptr_t)w64_arg(c, 0));
    if (!host) { k32_set_last_error(WIN_ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    uint64_t h = create_file(host, (uint32_t)w64_arg(c, 1), (uint32_t)w64_arg(c, 4));
    free(host);
    return h;
}

/* BOOL ReadFile(HANDLE, LPVOID, DWORD, LPDWORD read, LPOVERLAPPED) */
static uint64_t f_ReadFile(W64Ctx *c) {
    uint64_t h = w64_arg(c, 0);
    uint8_t *buf = (uint8_t *)(uintptr_t)w64_arg(c, 1);
    uint32_t want = (uint32_t)w64_arg(c, 2);
    uint32_t *got = (uint32_t *)(uintptr_t)w64_arg(c, 3);
    int fd;
    if (h == HANDLE_STDIN) fd = 0;
    else {
        Obj *o = k32_obj_from_handle(h);
        if (!o || o->type != OBJ_FILE) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }
        fd = o->fd;
    }
    ssize_t n;
    do { n = read(fd, buf, want); } while (n < 0 && errno == EINTR);
    if (n < 0) { k32_set_last_error(errno_to_win(errno)); if (got) *got = 0; return 0; }
    if (got) *got = (uint32_t)n;
    return 1;
}

/* BOOL WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD written, LPOVERLAPPED) */
static uint64_t f_WriteFile(W64Ctx *c) {
    uint64_t h = w64_arg(c, 0);
    const uint8_t *buf = (const uint8_t *)(uintptr_t)w64_arg(c, 1);
    uint32_t want = (uint32_t)w64_arg(c, 2);
    uint32_t *written = (uint32_t *)(uintptr_t)w64_arg(c, 3);
    int fd;
    if (h == HANDLE_STDOUT) fd = 1;
    else if (h == HANDLE_STDERR) fd = 2;
    else {
        Obj *o = k32_obj_from_handle(h);
        if (!o || o->type != OBJ_FILE) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }
        fd = o->fd;
    }
    uint32_t done = 0;
    while (done < want) {
        ssize_t n = write(fd, buf + done, want - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            k32_set_last_error(errno_to_win(errno));
            break;
        }
        done += (uint32_t)n;
    }
    if (written) *written = done;
    return done == want;
}

/* HANDLE GetStdHandle(DWORD): STD_INPUT -10, STD_OUTPUT -11, STD_ERROR -12. */
static uint64_t f_GetStdHandle(W64Ctx *c) {
    int32_t which = (int32_t)(uint32_t)w64_arg(c, 0);
    if (which == -10) return HANDLE_STDIN;
    if (which == -11) return HANDLE_STDOUT;
    if (which == -12) return HANDLE_STDERR;
    return INVALID_HANDLE_VALUE;
}

/* BOOL SetFilePointerEx(HANDLE, LARGE_INTEGER distance, PLARGE_INTEGER newpos, DWORD method) */
static uint64_t f_SetFilePointerEx(W64Ctx *c) {
    Obj *o = k32_obj_from_handle(w64_arg(c, 0));
    if (!o || o->type != OBJ_FILE) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }
    off_t off = (off_t)(int64_t)w64_arg(c, 1);
    int64_t *newpos = (int64_t *)(uintptr_t)w64_arg(c, 2);
    off_t r = lseek(o->fd, off, (int)(uint32_t)w64_arg(c, 3));
    if (r < 0) { k32_set_last_error(errno_to_win(errno)); return 0; }
    if (newpos) *newpos = (int64_t)r;
    return 1;
}

/* BOOL GetFileSizeEx(HANDLE, PLARGE_INTEGER) */
static uint64_t f_GetFileSizeEx(W64Ctx *c) {
    Obj *o = k32_obj_from_handle(w64_arg(c, 0));
    int64_t *size = (int64_t *)(uintptr_t)w64_arg(c, 1);
    if (!o || o->type != OBJ_FILE || !size) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }
    struct stat st;
    if (fstat(o->fd, &st) != 0) { k32_set_last_error(errno_to_win(errno)); return 0; }
    *size = (int64_t)st.st_size;
    return 1;
}

/* BOOL FlushFileBuffers(HANDLE) */
static uint64_t f_FlushFileBuffers(W64Ctx *c) {
    Obj *o = k32_obj_from_handle(w64_arg(c, 0));
    if (o && o->type == OBJ_FILE) fsync(o->fd);
    return 1;
}

/* BOOL CreateDirectoryW(LPCWSTR, LPSECURITY_ATTRIBUTES) */
static uint64_t f_CreateDirectoryW(W64Ctx *c) {
    char *host = host_path_w((const uint16_t *)(uintptr_t)w64_arg(c, 0));
    if (!host) { k32_set_last_error(WIN_ERROR_FILE_NOT_FOUND); return 0; }
    int r = mkdir(host, 0770);
    if (r != 0) k32_set_last_error(r == 0 ? 0 : (errno == EEXIST ? WIN_ERROR_ALREADY_EXISTS : errno_to_win(errno)));
    free(host);
    return r == 0;
}

/* DWORD GetFileAttributesW(LPCWSTR) */
static uint64_t f_GetFileAttributesW(W64Ctx *c) {
    char *host = host_path_w((const uint16_t *)(uintptr_t)w64_arg(c, 0));
    if (!host) return INVALID_FILE_ATTRIBUTES;
    struct stat st;
    int r = stat(host, &st);
    free(host);
    if (r != 0) { k32_set_last_error(errno_to_win(errno)); return INVALID_FILE_ATTRIBUTES; }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

/* BOOL DeleteFileW(LPCWSTR) */
static uint64_t f_DeleteFileW(W64Ctx *c) {
    char *host = host_path_w((const uint16_t *)(uintptr_t)w64_arg(c, 0));
    if (!host) return 0;
    int r = unlink(host);
    if (r != 0) k32_set_last_error(errno_to_win(errno));
    free(host);
    return r == 0;
}

/* BOOL SetThreadPriority(HANDLE, int): priorities are accepted and not applied. */
static uint64_t f_SetThreadPriority(W64Ctx *c) {
    (void)c;
    return 1;
}

/* int GetThreadPriority(HANDLE): THREAD_PRIORITY_NORMAL. */
static uint64_t f_GetThreadPriority(W64Ctx *c) {
    (void)c;
    return 0;
}

const HostExport kernel32_file_exports[] = {
    { "CreateFileW",            f_CreateFileW            },
    { "CreateFileA",            f_CreateFileA            },
    { "ReadFile",               f_ReadFile               },
    { "WriteFile",              f_WriteFile              },
    { "GetStdHandle",           f_GetStdHandle           },
    { "SetFilePointerEx",       f_SetFilePointerEx       },
    { "GetFileSizeEx",          f_GetFileSizeEx          },
    { "FlushFileBuffers",       f_FlushFileBuffers       },
    { "CreateDirectoryW",       f_CreateDirectoryW       },
    { "GetFileAttributesW",     f_GetFileAttributesW     },
    { "DeleteFileW",            f_DeleteFileW            },
    { "SetThreadPriority",      f_SetThreadPriority      },
    { "GetThreadPriority",      f_GetThreadPriority      },
    { NULL, NULL },
};

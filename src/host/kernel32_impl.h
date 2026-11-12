/* Internal helpers shared by the kernel32 implementation files. */
#ifndef XBEMU_KERNEL32_IMPL_H
#define XBEMU_KERNEL32_IMPL_H

#include "hostlib.h"

#include <pthread.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#define WIN_INFINITE        0xFFFFFFFFu
#define WIN_WAIT_OBJECT_0   0u
#define WIN_WAIT_TIMEOUT    0x102u
#define WIN_WAIT_FAILED     0xFFFFFFFFu
#define WIN_STILL_ACTIVE    259u

/* Win32 error codes used here. */
#define WIN_ERROR_INVALID_HANDLE   6u
#define WIN_ERROR_MOD_NOT_FOUND    126u
#define WIN_ERROR_PROC_NOT_FOUND   127u
#define WIN_ERROR_NOT_SUPPORTED    50u
#define WIN_ERROR_TIMEOUT          1460u
#define WIN_ERROR_FILE_NOT_FOUND   2u
#define WIN_ERROR_ACCESS_DENIED    5u
#define WIN_ERROR_FILE_EXISTS      80u
#define WIN_ERROR_ALREADY_EXISTS   183u

typedef enum ObjType { OBJ_EVENT = 1, OBJ_SEMAPHORE, OBJ_THREAD, OBJ_FILE } ObjType;

typedef struct Obj {
    ObjType type;
    pthread_mutex_t m;
    pthread_cond_t c;
    int signaled;        /* event: set; thread: finished */
    int manual_reset;    /* event only */
    long count, max;     /* semaphore only */
    /* thread only */
    pthread_t th;
    uint32_t tid;
    uint32_t exit_code;
    int finished;
    int suspended;
    uint64_t start, param;
    jmp_buf exit_jb;
    int fd;              /* file: host descriptor */
} Obj;

/* Handle objects: events, semaphores, threads, and files. The handle is the pointer. */
Obj *k32_obj_new(ObjType t);
Obj *k32_obj_from_handle(uint64_t h);

void k32_set_last_error(uint32_t err);
uint32_t k32_get_last_error(void);

/* Thread id of the calling thread (Windows-style DWORD). */
uint32_t k32_current_tid(void);

/* Convert a NUL-terminated UTF-16 string to ASCII, replacing non-ASCII with '?'. */
void k32_wide_to_ascii(const uint16_t *src, char *dst, size_t dstlen);

#endif

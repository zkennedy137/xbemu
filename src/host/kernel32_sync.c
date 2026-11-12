/*
 * kernel32 synchronization and thread objects.
 *
 * Simplifications, to be revisited:
 * - All SRW locks and condition variables share one process-wide mutex and
 *   condition variable. Correct, but every wake is a broadcast.
 * - Only exclusive SRW locks are supported. SRW shared mode is treated as exclusive.
 * - Handles are pointers to heap objects. Thread objects are not freed on CloseHandle.
 */
#define _DEFAULT_SOURCE
#include "kernel32_impl.h"
#include "../teb.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* ---- thread identity ---- */

static __thread uint32_t t_tid;

uint32_t k32_current_tid(void) {
    if (!t_tid) t_tid = (uint32_t)syscall(SYS_gettid);
    return t_tid;
}

/* ---- waiting helpers ---- */

/* Wait on a condition variable for at most ms milliseconds (WIN_INFINITE: forever).
 * Returns 0 when woken, ETIMEDOUT when the deadline passed. */
static int cond_wait_ms(pthread_cond_t *c, pthread_mutex_t *m, uint32_t ms) {
    if (ms == WIN_INFINITE) return pthread_cond_wait(c, m);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &ts);
}

/* ---- critical sections ----
 * CRITICAL_SECTION is 40 bytes on x64, the size of a glibc pthread_mutex_t.
 * It is initialized as a recursive mutex, which matches Windows semantics. */

_Static_assert(sizeof(pthread_mutex_t) <= 40, "CRITICAL_SECTION must hold a pthread mutex");

/* VOID InitializeCriticalSection(LPCRITICAL_SECTION) */
static uint64_t k32_InitializeCriticalSection(W64Ctx *c) {
    pthread_mutex_t *m = (pthread_mutex_t *)(uintptr_t)w64_arg(c, 0);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return 0;
}

static uint64_t k32_EnterCriticalSection(W64Ctx *c) {
    pthread_mutex_lock((pthread_mutex_t *)(uintptr_t)w64_arg(c, 0));
    return 0;
}

static uint64_t k32_LeaveCriticalSection(W64Ctx *c) {
    pthread_mutex_unlock((pthread_mutex_t *)(uintptr_t)w64_arg(c, 0));
    return 0;
}

static uint64_t k32_TryEnterCriticalSection(W64Ctx *c) {
    return pthread_mutex_trylock((pthread_mutex_t *)(uintptr_t)w64_arg(c, 0)) == 0;
}

static uint64_t k32_DeleteCriticalSection(W64Ctx *c) {
    pthread_mutex_destroy((pthread_mutex_t *)(uintptr_t)w64_arg(c, 0));
    return 0;
}

/* ---- SRW locks and condition variables ----
 * An SRWLOCK is a pointer-sized word: 0 = free, 1 = held exclusively.
 * A CONDITION_VARIABLE is a pointer-sized word that is not used.
 *
 * Waiters sleep on a stripe chosen by address: 256 mutex and condition-variable pairs
 * shared by all locks and condition variables. A waiter holds its stripe's mutex while it
 * releases the user lock, so a wakeup cannot be lost between the check and the sleep. */

#define SYNC_STRIPES 256
static pthread_mutex_t stripe_m[SYNC_STRIPES];
static pthread_cond_t stripe_c[SYNC_STRIPES];
static pthread_once_t stripes_once = PTHREAD_ONCE_INIT;

static void stripes_init(void) {
    for (int i = 0; i < SYNC_STRIPES; i++) {
        pthread_mutex_init(&stripe_m[i], NULL);
        pthread_cond_init(&stripe_c[i], NULL);
    }
}

static unsigned stripe_of(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return (unsigned)(((a >> 3) ^ (a >> 11) ^ (a >> 19)) % SYNC_STRIPES);
}

static void srw_acquire(volatile uint64_t *l) {
    pthread_once(&stripes_once, stripes_init);
    unsigned i = stripe_of((const void *)l);
    pthread_mutex_lock(&stripe_m[i]);
    while (*l) pthread_cond_wait(&stripe_c[i], &stripe_m[i]);
    *l = 1;
    pthread_mutex_unlock(&stripe_m[i]);
}

static void srw_release(volatile uint64_t *l) {
    pthread_once(&stripes_once, stripes_init);
    unsigned i = stripe_of((const void *)l);
    pthread_mutex_lock(&stripe_m[i]);
    *l = 0;
    pthread_cond_broadcast(&stripe_c[i]);
    pthread_mutex_unlock(&stripe_m[i]);
}

/* VOID AcquireSRWLockExclusive(PSRWLOCK) */
static uint64_t k32_AcquireSRWLockExclusive(W64Ctx *c) {
    srw_acquire((volatile uint64_t *)(uintptr_t)w64_arg(c, 0));
    return 0;
}

/* VOID ReleaseSRWLockExclusive(PSRWLOCK) */
static uint64_t k32_ReleaseSRWLockExclusive(W64Ctx *c) {
    srw_release((volatile uint64_t *)(uintptr_t)w64_arg(c, 0));
    return 0;
}

/* VOID InitializeConditionVariable(PCONDITION_VARIABLE) */
static uint64_t k32_InitializeConditionVariable(W64Ctx *c) {
    uint64_t *cv = (uint64_t *)(uintptr_t)w64_arg(c, 0);
    if (cv) *cv = 0;
    return 0;
}

/* VOID WakeConditionVariable / WakeAllConditionVariable (both wake every waiter on the stripe). */
static uint64_t k32_WakeConditionVariable(W64Ctx *c) {
    const void *cv = (const void *)(uintptr_t)w64_arg(c, 0);
    pthread_once(&stripes_once, stripes_init);
    unsigned i = stripe_of(cv);
    pthread_mutex_lock(&stripe_m[i]);
    pthread_cond_broadcast(&stripe_c[i]);
    pthread_mutex_unlock(&stripe_m[i]);
    return 0;
}

/* BOOL SleepConditionVariableCS(PCONDITION_VARIABLE, PCRITICAL_SECTION, DWORD ms) */
static uint64_t k32_SleepConditionVariableCS(W64Ctx *c) {
    const void *cv = (const void *)(uintptr_t)w64_arg(c, 0);
    pthread_mutex_t *cs = (pthread_mutex_t *)(uintptr_t)w64_arg(c, 1);
    uint32_t ms = (uint32_t)w64_arg(c, 2);
    pthread_once(&stripes_once, stripes_init);
    unsigned i = stripe_of(cv);

    pthread_mutex_lock(&stripe_m[i]);
    pthread_mutex_unlock(cs);
    int r = cond_wait_ms(&stripe_c[i], &stripe_m[i], ms);
    pthread_mutex_unlock(&stripe_m[i]);
    pthread_mutex_lock(cs);

    if (r == ETIMEDOUT) { k32_set_last_error(WIN_ERROR_TIMEOUT); return 0; }
    return 1;
}

/* BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE, PSRWLOCK, DWORD ms, ULONG flags)
 * The SRW lock is released (shared-mode waiters are treated as exclusive) and reacquired. */
static uint64_t k32_SleepConditionVariableSRW(W64Ctx *c) {
    const void *cv = (const void *)(uintptr_t)w64_arg(c, 0);
    volatile uint64_t *lock = (volatile uint64_t *)(uintptr_t)w64_arg(c, 1);
    uint32_t ms = (uint32_t)w64_arg(c, 2);
    pthread_once(&stripes_once, stripes_init);
    unsigned i = stripe_of(cv), j = stripe_of((const void *)lock);

    pthread_mutex_lock(&stripe_m[i]);
    if (i == j) {
        *lock = 0;
        pthread_cond_broadcast(&stripe_c[j]);
    } else {
        pthread_mutex_lock(&stripe_m[j]);
        *lock = 0;
        pthread_cond_broadcast(&stripe_c[j]);
        pthread_mutex_unlock(&stripe_m[j]);
    }
    int r = cond_wait_ms(&stripe_c[i], &stripe_m[i], ms);
    pthread_mutex_unlock(&stripe_m[i]);

    srw_acquire(lock);
    if (r == ETIMEDOUT) { k32_set_last_error(WIN_ERROR_TIMEOUT); return 0; }
    return 1;
}

/* Handle objects are shared with kernel32_file.c, so the type lives in kernel32_impl.h. */
Obj *k32_obj_new(ObjType t) {
    Obj *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->type = t;
    pthread_mutex_init(&o->m, NULL);
    pthread_cond_init(&o->c, NULL);
    return o;
}

/* Handles -1 (current process) and -2 (current thread) are pseudo handles. */
Obj *k32_obj_from_handle(uint64_t h) {
    if (h == 0 || h == (uint64_t)-1 || h == (uint64_t)-2) return NULL;
    return (Obj *)(uintptr_t)h;
}

/* HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCSTR name) */
static uint64_t k32_CreateEventA(W64Ctx *c) {
    Obj *o = k32_obj_new(OBJ_EVENT);
    if (!o) { k32_set_last_error(8); return 0; }
    o->manual_reset = (uint32_t)w64_arg(c, 1) != 0;
    o->signaled = (uint32_t)w64_arg(c, 2) != 0;
    return (uint64_t)(uintptr_t)o;
}

/* BOOL SetEvent(HANDLE) */
static uint64_t k32_SetEvent(W64Ctx *c) {
    Obj *o = k32_obj_from_handle(w64_arg(c, 0));
    if (!o || o->type != OBJ_EVENT) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }
    pthread_mutex_lock(&o->m);
    o->signaled = 1;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);
    return 1;
}

/* BOOL ResetEvent(HANDLE) */
static uint64_t k32_ResetEvent(W64Ctx *c) {
    Obj *o = k32_obj_from_handle(w64_arg(c, 0));
    if (!o || o->type != OBJ_EVENT) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }
    pthread_mutex_lock(&o->m);
    o->signaled = 0;
    pthread_mutex_unlock(&o->m);
    return 1;
}

/* HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES, LONG initial, LONG max, LPCSTR name) */
static uint64_t k32_CreateSemaphoreA(W64Ctx *c) {
    long initial = (int32_t)(uint32_t)w64_arg(c, 1);
    long max = (int32_t)(uint32_t)w64_arg(c, 2);
    if (max <= 0 || initial < 0 || initial > max) { k32_set_last_error(87); return 0; }
    Obj *o = k32_obj_new(OBJ_SEMAPHORE);
    if (!o) { k32_set_last_error(8); return 0; }
    o->count = initial;
    o->max = max;
    return (uint64_t)(uintptr_t)o;
}

/* BOOL ReleaseSemaphore(HANDLE, LONG count, LPLONG previous) */
static uint64_t k32_ReleaseSemaphore(W64Ctx *c) {
    Obj *o = k32_obj_from_handle(w64_arg(c, 0));
    long n = (int32_t)(uint32_t)w64_arg(c, 1);
    int32_t *prev = (int32_t *)(uintptr_t)w64_arg(c, 2);
    if (!o || o->type != OBJ_SEMAPHORE || n <= 0) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }

    pthread_mutex_lock(&o->m);
    if (prev) *prev = (int32_t)o->count;
    if (o->count + n > o->max) {
        pthread_mutex_unlock(&o->m);
        k32_set_last_error(298); /* ERROR_TOO_MANY_POSTS */
        return 0;
    }
    o->count += n;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);
    return 1;
}

/* ---- threads ---- */

static uint32_t next_tid = 0x1000;

/* Object of the thread running now, for ExitThread. NULL on the main thread. */
static __thread Obj *t_thread_obj;

static void *thread_main(void *arg) {
    Obj *o = arg;
    teb_init_current_thread();
    t_tid = o->tid;
    t_thread_obj = o;

    pthread_mutex_lock(&o->m);
    while (o->suspended) pthread_cond_wait(&o->c, &o->m);
    pthread_mutex_unlock(&o->m);

    /* ExitThread longjmps back here, which is the only safe way out of guest frames. */
    if (setjmp(o->exit_jb) == 0) {
        uint64_t ret = xbemu_call_w64((void *)(uintptr_t)o->start, o->param, 0, 0, 0);
        o->exit_code = (uint32_t)ret;
    }

    pthread_mutex_lock(&o->m);
    o->finished = 1;
    o->signaled = 1;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);
    return NULL;
}

#define CREATE_SUSPENDED 0x4u

/* HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T stack, LPTHREAD_START_ROUTINE, LPVOID param,
 *                     DWORD flags, LPDWORD tid) */
static uint64_t k32_CreateThread(W64Ctx *c) {
    Obj *o = k32_obj_new(OBJ_THREAD);
    if (!o) { k32_set_last_error(8); return 0; }
    o->start = w64_arg(c, 2);
    o->param = w64_arg(c, 3);
    o->suspended = ((uint32_t)w64_arg(c, 4) & CREATE_SUSPENDED) != 0;
    o->tid = __atomic_add_fetch(&next_tid, 4, __ATOMIC_SEQ_CST);

    if (pthread_create(&o->th, NULL, thread_main, o) != 0) {
        pthread_cond_destroy(&o->c);
        pthread_mutex_destroy(&o->m);
        free(o);
        k32_set_last_error(8);
        return 0;
    }
    uint32_t *tid = (uint32_t *)(uintptr_t)w64_arg(c, 5);
    if (tid) *tid = o->tid;
    return (uint64_t)(uintptr_t)o;
}

/* DWORD ResumeThread(HANDLE) */
static uint64_t k32_ResumeThread(W64Ctx *c) {
    Obj *o = k32_obj_from_handle(w64_arg(c, 0));
    if (!o || o->type != OBJ_THREAD) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return (uint32_t)-1; }
    pthread_mutex_lock(&o->m);
    int was = o->suspended;
    o->suspended = 0;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);
    return was ? 1 : 0;
}

/* BOOL GetExitCodeThread(HANDLE, LPDWORD) */
static uint64_t k32_GetExitCodeThread(W64Ctx *c) {
    Obj *o = k32_obj_from_handle(w64_arg(c, 0));
    uint32_t *code = (uint32_t *)(uintptr_t)w64_arg(c, 1);
    if (!o || o->type != OBJ_THREAD) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return 0; }
    pthread_mutex_lock(&o->m);
    if (code) *code = o->finished ? o->exit_code : WIN_STILL_ACTIVE;
    pthread_mutex_unlock(&o->m);
    return 1;
}

/* DWORD GetThreadId(HANDLE) */
static uint64_t k32_GetThreadId(W64Ctx *c) {
    Obj *o = k32_obj_from_handle(w64_arg(c, 0));
    if (!o || o->type != OBJ_THREAD) return 0;
    return o->tid;
}

/* VOID ExitThread(DWORD) */
static uint64_t k32_ExitThread(W64Ctx *c) {
    uint32_t code = (uint32_t)w64_arg(c, 0);
    if (!t_thread_obj) exit((int)code); /* main thread: end the process */
    t_thread_obj->exit_code = code;
    longjmp(t_thread_obj->exit_jb, 1);
}

/* BOOL CloseHandle(HANDLE) */
static uint64_t k32_CloseHandle(W64Ctx *c) {
    Obj *o = k32_obj_from_handle(w64_arg(c, 0));
    if (!o) return w64_arg(c, 0) == (uint64_t)-1 || w64_arg(c, 0) == (uint64_t)-2;
    if (o->type == OBJ_THREAD) {
        /* Leave the object alive: the thread may still be running. */
        pthread_detach(o->th);
        return 1;
    }
    if (o->type == OBJ_FILE) {
        if (o->fd > 2) close(o->fd);
        free(o);
        return 1;
    }
    pthread_cond_destroy(&o->c);
    pthread_mutex_destroy(&o->m);
    free(o);
    return 1;
}

/* BOOL DuplicateHandle(src proc, src, tgt proc, LPHANDLE tgt, ...)
 * Handles name shared objects, so duplicates just alias the same pointer. */
static uint64_t k32_DuplicateHandle(W64Ctx *c) {
    uint64_t src = w64_arg(c, 1);
    uint64_t *tgt = (uint64_t *)(uintptr_t)w64_arg(c, 3);
    if (tgt) *tgt = src;
    return 1;
}

/* Wait for one object. Caller holds no locks. */
static uint32_t wait_one(Obj *o, uint32_t ms) {
    pthread_mutex_lock(&o->m);
    switch (o->type) {
    case OBJ_EVENT:
    case OBJ_THREAD:
        while (!o->signaled) {
            if (cond_wait_ms(&o->c, &o->m, ms) == ETIMEDOUT) {
                pthread_mutex_unlock(&o->m);
                return WIN_WAIT_TIMEOUT;
            }
        }
        if (o->type == OBJ_EVENT && !o->manual_reset) o->signaled = 0;
        break;
    case OBJ_FILE:
        break; /* files are always ready */
    case OBJ_SEMAPHORE:
        while (o->count == 0) {
            if (cond_wait_ms(&o->c, &o->m, ms) == ETIMEDOUT) {
                pthread_mutex_unlock(&o->m);
                return WIN_WAIT_TIMEOUT;
            }
        }
        o->count--;
        break;
    }
    pthread_mutex_unlock(&o->m);
    return WIN_WAIT_OBJECT_0;
}

/* DWORD WaitForSingleObject(HANDLE, DWORD ms) */
static uint64_t k32_WaitForSingleObject(W64Ctx *c) {
    Obj *o = k32_obj_from_handle(w64_arg(c, 0));
    if (!o) { k32_set_last_error(WIN_ERROR_INVALID_HANDLE); return WIN_WAIT_FAILED; }
    return wait_one(o, (uint32_t)w64_arg(c, 1));
}

/* DWORD WaitForSingleObjectEx(HANDLE, DWORD ms, BOOL alertable): alertable APCs unsupported. */
static uint64_t k32_WaitForSingleObjectEx(W64Ctx *c) {
    return k32_WaitForSingleObject(c);
}

const HostExport kernel32_sync_exports[] = {
    { "InitializeCriticalSection",      k32_InitializeCriticalSection },
    { "EnterCriticalSection",           k32_EnterCriticalSection      },
    { "LeaveCriticalSection",           k32_LeaveCriticalSection      },
    { "TryEnterCriticalSection",        k32_TryEnterCriticalSection   },
    { "DeleteCriticalSection",          k32_DeleteCriticalSection     },
    { "AcquireSRWLockExclusive",        k32_AcquireSRWLockExclusive   },
    { "ReleaseSRWLockExclusive",        k32_ReleaseSRWLockExclusive   },
    { "InitializeConditionVariable",    k32_InitializeConditionVariable },
    { "WakeConditionVariable",          k32_WakeConditionVariable     },
    { "WakeAllConditionVariable",       k32_WakeConditionVariable     },
    { "SleepConditionVariableCS",       k32_SleepConditionVariableCS  },
    { "SleepConditionVariableSRW",      k32_SleepConditionVariableSRW },
    { "CreateEventA",                   k32_CreateEventA              },
    { "SetEvent",                       k32_SetEvent                  },
    { "ResetEvent",                     k32_ResetEvent                },
    { "CreateSemaphoreA",               k32_CreateSemaphoreA          },
    { "ReleaseSemaphore",               k32_ReleaseSemaphore          },
    { "CreateThread",                   k32_CreateThread              },
    { "ResumeThread",                   k32_ResumeThread              },
    { "GetExitCodeThread",              k32_GetExitCodeThread         },
    { "GetThreadId",                    k32_GetThreadId               },
    { "ExitThread",                     k32_ExitThread                },
    { "CloseHandle",                    k32_CloseHandle               },
    { "DuplicateHandle",                k32_DuplicateHandle           },
    { "WaitForSingleObject",            k32_WaitForSingleObject       },
    { "WaitForSingleObjectEx",          k32_WaitForSingleObjectEx     },
    { NULL, NULL },
};

/*
 * Thread Environment Block (TEB) and Process Environment Block (PEB).
 *
 * Windows code reads these through the GS segment on x64: gs:[0x30] is the TEB
 * self-pointer (NtCurrentTeb), gs:[0x60] is the PEB, gs:[0x08]/[0x10] are the
 * stack limits. Linux leaves GS at base 0, so each thread gets its own TEB and
 * GS base set with arch_prctl. glibc uses FS for its own TLS, so GS is free.
 *
 * Offsets (x64 TEB): NtTib.StackBase 0x08, StackLimit 0x10, Self 0x30,
 * ClientId.UniqueProcess 0x40, ClientId.UniqueThread 0x48, ProcessEnvironmentBlock 0x60.
 */
#define _GNU_SOURCE
#include "teb.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define TEB_SIZE 0x2000
#define PEB_SIZE 0x1000
#define ARCH_SET_GS 0x1001

#define OFF_STACK_BASE   0x08
#define OFF_STACK_LIMIT  0x10
#define OFF_SELF         0x30
#define OFF_PID          0x40
#define OFF_TID          0x48
#define OFF_PEB          0x60

static uint8_t *process_peb;

static void *map_zeroed(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static __thread int thread_ready;

void teb_init_current_thread(void) {
    if (thread_ready) return;

    if (!process_peb) process_peb = map_zeroed(PEB_SIZE);

    uint8_t *teb = map_zeroed(TEB_SIZE);
    if (!teb || !process_peb) return;
    thread_ready = 1;

    /* Stack bounds of the calling thread. */
    void *stack_lo = NULL;
    size_t stack_sz = 0;
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &stack_lo, &stack_sz);
        pthread_attr_destroy(&attr);
    }
    uint64_t lo = (uint64_t)(uintptr_t)stack_lo;
    uint64_t hi = lo + stack_sz;

    uint64_t self = (uint64_t)(uintptr_t)teb;
    uint64_t pid = (uint64_t)getpid();
    uint64_t tid = (uint64_t)syscall(SYS_gettid);
    uint64_t peb = (uint64_t)(uintptr_t)process_peb;

    memcpy(teb + OFF_STACK_BASE, &hi, 8);
    memcpy(teb + OFF_STACK_LIMIT, &lo, 8);
    memcpy(teb + OFF_SELF, &self, 8);
    memcpy(teb + OFF_PID, &pid, 8);
    memcpy(teb + OFF_TID, &tid, 8);
    memcpy(teb + OFF_PEB, &peb, 8);

    syscall(SYS_arch_prctl, ARCH_SET_GS, teb);
}

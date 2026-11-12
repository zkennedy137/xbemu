#define _GNU_SOURCE
#include "w64abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- Common thunk (entered from per-import stubs with rax = slot id) ---- */
__asm__(
    ".text\n"
    ".globl xbemu_thunk_common\n"
    "xbemu_thunk_common:\n"
    "  push %rbp\n"
    "  mov  %rsp, %rbp\n"
    "  sub  $0x160, %rsp\n"          /* 352 = W64Ctx size: keeps rsp 16-byte aligned */
    "  mov  %rcx, 0(%rsp)\n"
    "  mov  %rdx, 8(%rsp)\n"
    "  mov  %r8, 16(%rsp)\n"
    "  mov  %r9, 24(%rsp)\n"
    "  lea  16(%rbp), %r10\n"        /* shadow space = caller's stack args base */
    "  mov  %r10, 32(%rsp)\n"
    "  mov  %rax, 40(%rsp)\n"        /* slot id */
    "  mov  %rdi, 48(%rsp)\n"
    "  mov  %rsi, 56(%rsp)\n"
    "  movups %xmm0, 64(%rsp)\n"     /* float args / float return slot */
    "  movups %xmm1, 80(%rsp)\n"
    "  movups %xmm2, 96(%rsp)\n"
    "  movups %xmm3, 112(%rsp)\n"
    "  mov  %rbx, 128(%rsp)\n"       /* guest callee-saved state, for unwinding */
    "  mov  (%rbp), %r11\n"
    "  mov  %r11, 136(%rsp)\n"       /* guest rbp (pushed by the thunk) */
    "  mov  %r12, 144(%rsp)\n"
    "  mov  %r13, 152(%rsp)\n"
    "  mov  %r14, 160(%rsp)\n"
    "  mov  %r15, 168(%rsp)\n"
    "  mov  8(%rbp), %r11\n"
    "  mov  %r11, 176(%rsp)\n"       /* guest return address */
    "  lea  16(%rbp), %r11\n"
    "  mov  %r11, 184(%rsp)\n"       /* guest rsp after return */
    "  movups %xmm6, 192(%rsp)\n"    /* Win64 callee-saved, may be clobbered by host code */
    "  movups %xmm7, 208(%rsp)\n"
    "  movups %xmm8, 224(%rsp)\n"
    "  movups %xmm9, 240(%rsp)\n"
    "  movups %xmm10,256(%rsp)\n"
    "  movups %xmm11,272(%rsp)\n"
    "  movups %xmm12,288(%rsp)\n"
    "  movups %xmm13,304(%rsp)\n"
    "  movups %xmm14,320(%rsp)\n"
    "  movups %xmm15,336(%rsp)\n"
    "  mov  %rsp, %rdi\n"
    "  call w64_dispatch\n"          /* rax = integer return value */
    "  movups 64(%rsp), %xmm0\n"     /* float return value, if the host set one */
    "  movups 192(%rsp), %xmm6\n"
    "  movups 208(%rsp), %xmm7\n"
    "  movups 224(%rsp), %xmm8\n"
    "  movups 240(%rsp), %xmm9\n"
    "  movups 256(%rsp), %xmm10\n"
    "  movups 272(%rsp), %xmm11\n"
    "  movups 288(%rsp), %xmm12\n"
    "  movups 304(%rsp), %xmm13\n"
    "  movups 320(%rsp), %xmm14\n"
    "  movups 336(%rsp), %xmm15\n"
    "  mov  48(%rsp), %rdi\n"
    "  mov  56(%rsp), %rsi\n"
    "  leave\n"
    "  ret\n"

    /* uint64_t xbemu_call_w64(fn, a1, a2, a3, a4): SysV in, Win64 call out.
     * The xbemu_call_w64_start/_end labels bound it for the exception unwinder. */
    ".globl xbemu_call_w64_start\n"
    "xbemu_call_w64_start:\n"
    ".globl xbemu_call_w64\n"
    "xbemu_call_w64:\n"
    "  push %rbp\n"
    "  mov  %rsp, %rbp\n"
    "  mov  %r8, %r9\n"              /* a4 -> r9 */
    "  mov  %rcx, %r8\n"             /* a3 -> r8 */
    "  mov  %rsi, %rcx\n"            /* a1 -> rcx */
    "  mov  %rdi, %rax\n"            /* fn */
    "  sub  $32, %rsp\n"             /* shadow space; rsp is 16-byte aligned here */
    "  call *%rax\n"
    "  leave\n"
    "  ret\n"
    ".globl xbemu_call_w64_end\n"
    "xbemu_call_w64_end:\n"
);

/* Per-thread record of host functions that guest code called (innermost last). A host
 * function may call back into guest code, and an exception escaping that callback resumes
 * the search at the guest frame recorded here. */
#define CALL_DEPTH_MAX 256
static __thread W64Ctx *call_frames[CALL_DEPTH_MAX];
static __thread int call_depth;

int w64_call_depth(void) { return call_depth; }
void w64_call_depth_set(int d) { if (d >= 0 && d <= CALL_DEPTH_MAX) call_depth = d; }
W64Ctx *w64_call_frame(int k) { return k >= 0 && k < call_depth ? call_frames[k] : NULL; }

/* Drop records of host frames that an unwind abandoned: those below the landing frame. */
void w64_call_trim(uint64_t landing_rsp) {
    while (call_depth > 0 && call_frames[call_depth - 1]->rsp < landing_rsp) call_depth--;
}

extern void xbemu_thunk_common(void);

/* ---- Slot table and stub region ---- */

#define STUB_SIZE 32
#define STUB_REGION_BYTES (1024 * 1024)
#define MAX_SLOTS (STUB_REGION_BYTES / STUB_SIZE)

typedef struct Slot {
    char *dll;
    char *name;
    W64Fn fn;    /* NULL = unimplemented */
    void *user;  /* handler data, e.g. the host function a forwarder calls */
} Slot;

static Slot slots[MAX_SLOTS];
static size_t nslots;
/* Stubs are executed by any thread while new ones are being written. The region is
 * therefore mapped twice from one memfd: a writable view for creating stubs, and an
 * executable view that never changes protection. (Changing the protection of the
 * executable view while another thread runs a stub faults that thread.) */
static uint8_t *stub_region;   /* executable view */
static uint8_t *stub_writer;   /* writable view of the same memory */

static int stub_region_init(void) {
    int fd = memfd_create("xbemu-stubs", 0);
    if (fd < 0) return -1;
    if (ftruncate(fd, STUB_REGION_BYTES) != 0) { close(fd); return -1; }
    stub_writer = mmap(NULL, STUB_REGION_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    stub_region = mmap(NULL, STUB_REGION_BYTES, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    close(fd);
    if (stub_writer == MAP_FAILED || stub_region == MAP_FAILED) {
        if (stub_writer != MAP_FAILED) munmap(stub_writer, STUB_REGION_BYTES);
        if (stub_region != MAP_FAILED) munmap(stub_region, STUB_REGION_BYTES);
        stub_writer = stub_region = NULL;
        return -1;
    }
    return 0;
}

void *w64_make_stub(const char *dll, const char *name, W64Fn fn) {
    return w64_make_stub_user(dll, name, fn, NULL);
}

void *w64_slot_user(const W64Ctx *c) {
    return c->id < nslots ? slots[c->id].user : NULL;
}

void *w64_make_stub_user(const char *dll, const char *name, W64Fn fn, void *user) {
    if (!stub_region && stub_region_init() != 0)
        return NULL;
    if (nslots >= MAX_SLOTS)
        return NULL;

    size_t id = nslots++;
    Slot *s = &slots[id];
    s->dll = strdup(dll);
    s->name = strdup(name);
    s->fn = fn;
    s->user = user;
    if (!s->dll || !s->name)
        return NULL;

    /* Stub layout (32 bytes):
     *   b8 <id32>          mov  eax, id
     *   ff 25 00 00 00 00  jmp  [rip+0]
     *   <8-byte thunk address>
     */
    uint8_t *p = stub_writer + id * STUB_SIZE;      /* write through the writable view */
    uint8_t *exec = stub_region + id * STUB_SIZE;   /* the address guest code calls */
    uint64_t target = (uint64_t)(uintptr_t)xbemu_thunk_common;
    uint32_t id32 = (uint32_t)id;

    p[0] = 0xB8;
    memcpy(p + 1, &id32, 4);
    p[5] = 0xFF;
    p[6] = 0x25;
    memset(p + 7, 0, 4);
    memcpy(p + 11, &target, 8);
    memset(p + 19, 0xCC, STUB_SIZE - 19);

    return exec;
}

/* Called by the thunk with the saved Win64 register state. */
uint64_t w64_dispatch(W64Ctx *c) {
    if (c->id >= nslots) {
        fprintf(stderr, "[xbemu] bad import slot %llu\n", (unsigned long long)c->id);
        return 0;
    }
    Slot *s = &slots[c->id];
    if (!s->fn) {
        fprintf(stderr, "[xbemu] unimplemented import %s!%s\n", s->dll, s->name);
        return 0;
    }
    if (call_depth < CALL_DEPTH_MAX) call_frames[call_depth++] = c;
    uint64_t rc = s->fn(c);
    if (call_depth > 0) call_depth--;
    return rc;
}

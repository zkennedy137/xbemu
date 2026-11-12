/*
 * Win64 <-> SysV ABI bridge.
 *
 * Guest (PE) code uses the Microsoft x64 calling convention:
 *   args: rcx, rdx, r8, r9, then stack; 32 bytes of shadow space.
 *   Floating-point args: xmm0-xmm3, by position (a double in position 1 is in xmm1).
 *   callee-saved: rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15.
 * Host code (this emulator) uses System V: rdi, rsi, rdx, rcx, r8, r9.
 *
 * Each import gets a small machine-code stub that loads its slot id into rax
 * and jumps to a common thunk. The thunk saves the Win64 argument registers
 * into a W64Ctx and calls w64_dispatch(), which calls the host implementation.
 */
#ifndef XBEMU_W64ABI_H
#define XBEMU_W64ABI_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Field offsets are hard-coded in the assembly in w64abi.c. Do not reorder. */
typedef struct W64Ctx {
    uint64_t r[4];      /* 0:   rcx, rdx, r8, r9 */
    uint64_t *stack;    /* 32:  start of the caller's shadow space (arg 0 slot) */
    uint64_t id;        /* 40:  import slot id */
    uint64_t rdi, rsi;  /* 48, 56: preserved across the host call */
    uint64_t xmm[4][2]; /* 64:  xmm0-xmm3 in. Host sets xmm[0] for a float result. */
    /* Guest state at the call, for exception unwinding (see seh.c). */
    uint64_t rbx, rbp, r12, r13, r14, r15; /* 128..175 */
    uint64_t rip;      /* 176: return address into the guest */
    uint64_t rsp;      /* 184: guest rsp after the call returns */
    uint64_t xmm_nv[10][2]; /* 192..351: xmm6-xmm15 */
} W64Ctx;

typedef uint64_t (*W64Fn)(W64Ctx *c);

/* Win64 integer argument n (0-based). Args 0-3 are registers; args 4+ are on the stack. */
static inline uint64_t w64_arg(const W64Ctx *c, unsigned n) {
    return n < 4 ? c->r[n] : c->stack[n];
}

/* Win64 floating-point argument in position n (0-3), passed in xmm n. */
static inline double w64_arg_f64(const W64Ctx *c, unsigned n) {
    double d;
    memcpy(&d, &c->xmm[n & 3][0], 8);
    return d;
}

/* Win64 float (32-bit) argument in position n (0-3). */
static inline float w64_arg_f32(const W64Ctx *c, unsigned n) {
    float f;
    memcpy(&f, &c->xmm[n & 3][0], 4);
    return f;
}

/* Set a floating-point return value (returned in xmm0). */
static inline void w64_ret_f64(W64Ctx *c, double d) {
    memcpy(&c->xmm[0][0], &d, 8);
}

/*
 * Create a stub that calls fn when the guest calls it. If fn is NULL, the stub
 * logs "unimplemented import dll!name" and returns 0. Returns the stub address,
 * or NULL on failure.
 */
void *w64_make_stub(const char *dll, const char *name, W64Fn fn);

/* Like w64_make_stub, but also stores `user`, which the handler reads with w64_slot_user. */
void *w64_make_stub_user(const char *dll, const char *name, W64Fn fn, void *user);

/* The `user` pointer of the stub that is being called. */
void *w64_slot_user(const W64Ctx *c);

/* Called by the thunk. Not for direct use. */
uint64_t w64_dispatch(W64Ctx *c);

/* Exception-unwinding support (seh.c): the host calls made by guest code, innermost last. */
int w64_call_depth(void);
void w64_call_depth_set(int d);
W64Ctx *w64_call_frame(int k);
void w64_call_trim(uint64_t landing_rsp);

/* Call a Win64-ABI function pointer with up to four integer args. */
uint64_t xbemu_call_w64(void *fn, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);

#endif

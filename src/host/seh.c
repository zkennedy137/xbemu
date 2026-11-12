/*
 * Structured exception handling for guest x64 code: RaiseException and the Rtl unwind
 * functions that MinGW's C++ runtime uses. The unwinder follows the Win64 model:
 *
 *   - Each PE function has a RUNTIME_FUNCTION in the module's exception directory,
 *     which points to UNWIND_INFO (x64 unwind codes, plus an optional language handler).
 *   - RaiseException does the search pass: it walks the frames and calls each frame's
 *     language handler until one returns ExceptionContinueExecution or a handler lands
 *     in a catch block with RtlUnwindEx.
 *   - RtlUnwindEx does the unwind pass: it runs unwind handlers (cleanup, destructors)
 *     in each frame it leaves, then restores the target frame's registers and jumps
 *     to the landing pad.
 *
 * Limits: the walk stops at the first frame outside a PE module, so exceptions do not
 * propagate through host code. Floating-point state is restored only for xmm6-xmm15.
 *
 * CONTEXT field offsets (x64) are verified against the MinGW headers:
 *   Rax 0x78 (register n is at 0x78 + 8n), Rsp 0x98, Rbp 0xa0, Rdi 0xb0, Rip 0xf8,
 *   Xmm0 0x1a0 (16 bytes each), size 0x4d0.
 */
#define _DEFAULT_SOURCE
#include "kernel32_impl.h"
#include "../pe.h"

#include <stdint.h>
#include <string.h>

#define CTX_SIZE   0x4d0u
#define CTX_RSP    0x98u
#define CTX_RBP    0xa0u
#define CTX_RIP    0xf8u
#define CTX_XMM0   0x1a0u
#define CTX_REG(n) (0x78u + 8u * (unsigned)(n))

#define REG_RCX 1
#define REG_RDX 2
#define REG_RBX 3
#define REG_RBP 5
#define REG_RSI 6
#define REG_RDI 7
#define REG_R8  8
#define REG_R9  9
#define REG_R12 12
#define REG_R15 15

#define UNW_FLAG_EHANDLER  0x1
#define UNW_FLAG_UHANDLER  0x2
#define UNW_FLAG_CHAININFO 0x4

#define EXCEPTION_NONCONTINUABLE 0x1
#define EXCEPTION_GCC_FORCED     0x21474343u
#define EXCEPTION_UNWINDING      0x2
#define EXCEPTION_TARGET_UNWIND  0x20
#define EXCEPTION_MAXIMUM_PARAMETERS 15
#define EXC_CONTINUE_EXECUTION   0

/* Bounds of xbemu_call_w64 (w64abi.c): the host-to-guest callback trampoline. */
extern char xbemu_call_w64_start[], xbemu_call_w64_end[];

/* Restore every register from a CONTEXT and jump to its Rip. Does not return. */
extern void xbemu_restore_context(const uint8_t *ctx) __attribute__((noreturn));

__asm__(
    ".text\n"
    ".globl xbemu_restore_context\n"
    "xbemu_restore_context:\n"       /* rdi = CONTEXT */
    "  movups 0x200(%rdi), %xmm6\n"
    "  movups 0x210(%rdi), %xmm7\n"
    "  movups 0x220(%rdi), %xmm8\n"
    "  movups 0x230(%rdi), %xmm9\n"
    "  movups 0x240(%rdi), %xmm10\n"
    "  movups 0x250(%rdi), %xmm11\n"
    "  movups 0x260(%rdi), %xmm12\n"
    "  movups 0x270(%rdi), %xmm13\n"
    "  movups 0x280(%rdi), %xmm14\n"
    "  movups 0x290(%rdi), %xmm15\n"
    "  mov 0xf8(%rdi), %r10\n"       /* rip */
    "  mov 0x78(%rdi), %rax\n"
    "  mov 0x80(%rdi), %rcx\n"
    "  mov 0x88(%rdi), %rdx\n"
    "  mov 0x90(%rdi), %rbx\n"
    "  mov 0xa0(%rdi), %rbp\n"
    "  mov 0xa8(%rdi), %rsi\n"
    "  mov 0xb8(%rdi), %r8\n"
    "  mov 0xc0(%rdi), %r9\n"
    "  mov 0xd8(%rdi), %r12\n"
    "  mov 0xe0(%rdi), %r13\n"
    "  mov 0xe8(%rdi), %r14\n"
    "  mov 0xf0(%rdi), %r15\n"
    "  mov 0x98(%rdi), %rsp\n"
    "  mov 0xb0(%rdi), %rdi\n"       /* last: rdi is the base register above */
    "  jmp *%r10\n"
);

static uint64_t cget(const uint8_t *c, unsigned off) {
    uint64_t v;
    memcpy(&v, c + off, 8);
    return v;
}

static void cset(uint8_t *c, unsigned off, uint64_t v) {
    memcpy(c + off, &v, 8);
}

/* Read from unwind data, which must lie inside the module. */
static int mod_read(PeModule *m, uint64_t rva, void *dst, size_t n) {
    if (rva > m->size || n > m->size - rva) return 0;
    memcpy(dst, m->base + rva, n);
    return 1;
}

typedef struct Frame {
    uint64_t est;      /* establisher frame: rsp of this frame before unwinding */
    uint64_t handler;  /* language or unwind handler (absolute), or 0 */
    uint64_t hdata;    /* handler data (absolute) */
    unsigned flags;    /* UNW_FLAG_* from the unwind info */
} Frame;

/*
 * Unwind one frame described by rf, updating ctx to the caller's state.
 * Applies only the unwind codes that have executed at pc (the prolog may be partly run).
 */
static int virtual_unwind(PeModule *m, uint8_t *ctx, uint64_t pc, const uint8_t *rf, Frame *out) {
    uint32_t begin, ui_rva;
    memcpy(&begin, rf, 4);
    memcpy(&ui_rva, rf + 8, 4);

    uint64_t pc_off = pc - (uint64_t)(uintptr_t)m->base - begin;
    out->est = cget(ctx, CTX_RSP);
    out->handler = 0;
    out->hdata = 0;
    out->flags = 0;

    int first = 1;
    for (int chain = 0; chain < 8; chain++) {
        uint8_t hdr[4];
        if (!mod_read(m, ui_rva, hdr, 4)) return 0;
        unsigned uflags = hdr[0] >> 3;
        unsigned count = hdr[2];
        unsigned freg = hdr[3] & 0xF;
        unsigned foff = hdr[3] >> 4;
        uint64_t codes_rva = ui_rva + 4;
        uint64_t after = 4 + 2 * (uint64_t)((count + 1) & ~1u);
        uint64_t limit = first ? pc_off : ~0ull;

        int popped = 0;
        for (unsigned i = 0; i < count;) {
            uint8_t code[2];
            if (!mod_read(m, codes_rva + 2 * (uint64_t)i, code, 2)) return 0;
            uint8_t co = code[0];
            unsigned op = code[1] & 0xF, info = code[1] >> 4;
            unsigned slots = 1;
            if (op == 1) slots = info ? 3 : 2;      /* UWOP_ALLOC_LARGE */
            else if (op == 4 || op == 8) slots = 2; /* UWOP_SAVE_NONVOL, UWOP_SAVE_XMM128 */
            else if (op == 5 || op == 9) slots = 3; /* the FAR variants */

            if (co <= limit) {
                uint64_t rsp = cget(ctx, CTX_RSP);
                uint8_t ext[4] = { 0 };
                uint64_t ext_rva = codes_rva + 2 * (uint64_t)(i + 1);
                switch (op) {
                case 0: { /* UWOP_PUSH_NONVOL */
                    uint64_t v;
                    memcpy(&v, (const void *)(uintptr_t)rsp, 8);
                    cset(ctx, CTX_REG(info), v);
                    rsp += 8;
                    break;
                }
                case 1: { /* UWOP_ALLOC_LARGE */
                    if (!mod_read(m, ext_rva, ext, info ? 4 : 2)) return 0;
                    uint64_t size = info ? (uint64_t)*(uint32_t *)ext
                                         : (uint64_t)*(uint16_t *)ext * 8;
                    rsp += size;
                    break;
                }
                case 2: /* UWOP_ALLOC_SMALL */
                    rsp += (uint64_t)info * 8 + 8;
                    break;
                case 3: /* UWOP_SET_FPREG */
                    rsp = cget(ctx, CTX_REG(freg)) - (uint64_t)foff * 16;
                    break;
                case 4: { /* UWOP_SAVE_NONVOL: register saved at rsp + 8*slot */
                    if (!mod_read(m, ext_rva, ext, 2)) return 0;
                    uint64_t off = (uint64_t)*(uint16_t *)ext * 8;
                    uint64_t v;
                    memcpy(&v, (const void *)(uintptr_t)(rsp + off), 8);
                    cset(ctx, CTX_REG(info), v);
                    break;
                }
                case 5: { /* UWOP_SAVE_NONVOL_FAR */
                    if (!mod_read(m, ext_rva, ext, 4)) return 0;
                    uint64_t off = *(uint32_t *)ext;
                    uint64_t v;
                    memcpy(&v, (const void *)(uintptr_t)(rsp + off), 8);
                    cset(ctx, CTX_REG(info), v);
                    break;
                }
                case 8: { /* UWOP_SAVE_XMM128 */
                    if (!mod_read(m, ext_rva, ext, 2)) return 0;
                    uint64_t off = (uint64_t)*(uint16_t *)ext * 16;
                    memcpy(ctx + CTX_XMM0 + 16 * info, (const void *)(uintptr_t)(rsp + off), 16);
                    break;
                }
                case 9: { /* UWOP_SAVE_XMM128_FAR */
                    if (!mod_read(m, ext_rva, ext, 4)) return 0;
                    uint64_t off = *(uint32_t *)ext;
                    memcpy(ctx + CTX_XMM0 + 16 * info, (const void *)(uintptr_t)(rsp + off), 16);
                    break;
                }
                case 10: { /* UWOP_PUSH_MACHFRAME: an interrupt frame */
                    uint64_t nrip, nrsp;
                    memcpy(&nrip, (const void *)(uintptr_t)(rsp + (info ? 8 : 0)), 8);
                    memcpy(&nrsp, (const void *)(uintptr_t)(rsp + (info ? 32 : 24)), 8);
                    cset(ctx, CTX_RIP, nrip);
                    cset(ctx, CTX_RSP, nrsp);
                    popped = 1;
                    break;
                }
                default: /* epilog and spare codes do not change state while unwinding */
                    break;
                }
                if (op != 10) cset(ctx, CTX_RSP, rsp);
            }
            i += slots;
        }

        if (uflags & UNW_FLAG_CHAININFO) {
            /* The unwind info continues in a chained RUNTIME_FUNCTION after the codes. */
            uint8_t crf[12];
            if (!mod_read(m, ui_rva + after, crf, 12)) return 0;
            memcpy(&ui_rva, crf + 8, 4);
            first = 0;
            continue;
        }

        if (uflags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) {
            uint32_t hrva;
            if (!mod_read(m, ui_rva + after, &hrva, 4)) return 0;
            out->handler = (uint64_t)(uintptr_t)m->base + hrva;
            out->hdata = (uint64_t)(uintptr_t)m->base + ui_rva + after + 4;
            out->flags = uflags;
        }

        if (!popped) {
            /* Return address is at the top of the stack. */
            uint64_t rsp = cget(ctx, CTX_RSP), rip;
            memcpy(&rip, (const void *)(uintptr_t)rsp, 8);
            cset(ctx, CTX_RIP, rip);
            cset(ctx, CTX_RSP, rsp + 8);
        }
        return 1;
    }
    return 0;
}

/* Unwind one frame whether or not it has unwind data. Returns 0 if the walk must stop. */
static int step_frame(uint8_t *ctx, uint64_t pc, PeModule *m, Frame *f) {
    const uint8_t *rf = pe_runtime_function(m, pc);
    if (rf)
        return virtual_unwind(m, ctx, pc, rf, f);

    /* Leaf function: the return address is at the top of the stack. */
    uint64_t rsp = cget(ctx, CTX_RSP), rip;
    memcpy(&rip, (const void *)(uintptr_t)rsp, 8);
    f->est = rsp;
    f->handler = 0;
    f->hdata = 0;
    f->flags = 0;
    cset(ctx, CTX_RIP, rip);
    cset(ctx, CTX_RSP, rsp + 8);
    return 1;
}

/* DISPATCHER_CONTEXT (80 bytes, offsets verified against the MinGW headers). */
static void fill_dispatch(uint8_t *dc, uint64_t control_pc, PeModule *m, const uint8_t *rf,
                          const Frame *f, uint64_t target_ip, const uint8_t *ctxrec) {
    uint64_t v;
    memset(dc, 0, 80);
    v = control_pc;                            memcpy(dc + 0x00, &v, 8);
    v = (uint64_t)(uintptr_t)m->base;          memcpy(dc + 0x08, &v, 8);
    v = (uint64_t)(uintptr_t)rf;               memcpy(dc + 0x10, &v, 8);
    v = f->est;                                memcpy(dc + 0x18, &v, 8);
    v = target_ip;                             memcpy(dc + 0x20, &v, 8);
    v = (uint64_t)(uintptr_t)ctxrec;           memcpy(dc + 0x28, &v, 8);
    v = f->handler;                            memcpy(dc + 0x30, &v, 8);
    v = f->hdata;                              memcpy(dc + 0x38, &v, 8);
}

/* Build an EXCEPTION_RECORD (152 bytes; offsets verified). */
static void fill_record(uint8_t *er, uint32_t code, uint32_t flags, uint64_t addr,
                        uint32_t nargs, const uint64_t *args) {
    memset(er, 0, 152);
    memcpy(er + 0, &code, 4);
    memcpy(er + 4, &flags, 4);
    memcpy(er + 16, &addr, 8);
    if (nargs > EXCEPTION_MAXIMUM_PARAMETERS) nargs = EXCEPTION_MAXIMUM_PARAMETERS;
    memcpy(er + 24, &nargs, 4);
    if (args) memcpy(er + 32, args, 8 * nargs);
}

/* The exception whose search pass is running, for nested forced unwinds. */
static uint8_t *active_ctx;
static uint8_t *active_er;

/* Unwind pass, declared here for the search pass below. */
static void unwind_to(uint64_t target, uint64_t target_ip, uint8_t *er, uint64_t retval,
                      const uint8_t *orig);

/*
 * A guest callback called from host code (xbemu_call_w64) has host frames below it, which
 * have no unwind data. When the walk reaches one, it resumes at the guest frame that called
 * the host function: the thunk's record with the lowest guest rsp above the boundary. The
 * boundary is the rsp of the trampoline's frame. Records of the throw's own host calls
 * lie below it and are skipped.
 */
static int hop_to_host_caller(uint8_t *work) {
    uint64_t pc = cget(work, CTX_RIP);
    if (!(pc >= (uint64_t)(uintptr_t)xbemu_call_w64_start && pc < (uint64_t)(uintptr_t)xbemu_call_w64_end))
        return 0;
    uint64_t boundary = cget(work, CTX_RSP);
    W64Ctx *best = NULL;
    for (int k = 0; k < w64_call_depth(); k++) {
        W64Ctx *c = w64_call_frame(k);
        if (c && c->rsp > boundary && (!best || c->rsp < best->rsp)) best = c;
    }
    if (!best) return 0;
    cset(work, CTX_RIP, best->rip);
    cset(work, CTX_RSP, best->rsp);
    cset(work, CTX_RBP, best->rbp);
    cset(work, CTX_REG(REG_RBX), best->rbx);
    cset(work, CTX_REG(REG_RSI), best->rsi);
    cset(work, CTX_REG(REG_RDI), best->rdi);
    cset(work, CTX_REG(REG_R12), best->r12);
    cset(work, CTX_REG(REG_R12 + 1), best->r13);
    cset(work, CTX_REG(REG_R12 + 2), best->r14);
    cset(work, CTX_REG(REG_R15), best->r15);
    memcpy(work + CTX_XMM0 + 6 * 16, best->xmm_nv, 10 * 16);
    return 1;
}

/* Search pass: call language handlers from the throw site outward. */
static void dispatch_exception(const uint8_t *orig, uint8_t *er) {
    uint8_t work[CTX_SIZE], ctxcopy[CTX_SIZE];
    memcpy(work, orig, CTX_SIZE);
    memcpy(ctxcopy, orig, CTX_SIZE);
    uint8_t *saved_ctx = active_ctx, *saved_er = active_er;
    active_ctx = ctxcopy;
    active_er = er;

    for (int depth = 0; depth < 1024; depth++) {
        uint64_t pc = cget(work, CTX_RIP);
        PeModule *m = pe_find_module_by_addr((const void *)(uintptr_t)pc);
        if (!m) {
            /* Host code: resume at the guest caller if this is a host-to-guest boundary. */
            if (hop_to_host_caller(work)) continue;
            break;
        }

        const uint8_t *rf = pe_runtime_function(m, pc);
        Frame f;
        if (!step_frame(work, pc, m, &f)) break;

        if (f.handler && (f.flags & UNW_FLAG_EHANDLER)) {
            uint8_t dc[80];
            fill_dispatch(dc, pc, m, rf, &f, 0, ctxcopy);
            uint64_t disp = xbemu_call_w64((void *)(uintptr_t)f.handler, (uint64_t)(uintptr_t)er,
                                           f.est, (uint64_t)(uintptr_t)ctxcopy,
                                           (uint64_t)(uintptr_t)dc);
            if (disp == EXC_CONTINUE_EXECUTION) { /* the handler fixed the fault */
                active_ctx = saved_ctx;
                active_er = saved_er;
                return;
            }
        }
        if (cget(work, CTX_RIP) == 0) break;
    }
    active_ctx = saved_ctx;
    active_er = saved_er;
}

/* Resume in the target frame described by cur. Volatile registers come from ctx_rec. */
static void land(uint8_t *cur, const uint8_t *ctx_rec, uint64_t target_ip, uint64_t retval)
    __attribute__((noreturn));
static void land(uint8_t *cur, const uint8_t *ctx_rec, uint64_t target_ip, uint64_t retval) {
    static const unsigned volatile_regs[] = { 1, 2, 8, 9, 10, 11 }; /* rcx rdx r8 r9 r10 r11 */
    for (unsigned i = 0; i < sizeof volatile_regs / sizeof volatile_regs[0]; i++)
        cset(cur, CTX_REG(volatile_regs[i]), cget(ctx_rec, CTX_REG(volatile_regs[i])));
    memcpy(cur + CTX_XMM0, ctx_rec + CTX_XMM0, 6 * 16); /* xmm0-xmm5 */
    cset(cur, CTX_REG(0), retval);
    cset(cur, CTX_RIP, target_ip);
    w64_call_trim(cget(cur, CTX_RSP));   /* host calls abandoned by this unwind */
    xbemu_restore_context(cur);
}

/* Unwind pass: run unwind handlers below target, then land in the target frame. */
static void unwind_to(uint64_t target, uint64_t target_ip, uint8_t *er, uint64_t retval,
                      const uint8_t *orig) {
    uint8_t work[CTX_SIZE], ctxcopy[CTX_SIZE];
    memcpy(work, orig, CTX_SIZE);
    memcpy(ctxcopy, orig, CTX_SIZE);
    uint32_t flags = EXCEPTION_UNWINDING;
    memcpy(er + 4, &flags, 4);

    for (int depth = 0; depth < 1024; depth++) {
        uint64_t pc = cget(work, CTX_RIP);
        PeModule *m = pe_find_module_by_addr((const void *)(uintptr_t)pc);
        if (!m) {
            if (hop_to_host_caller(work)) continue;
            break;
        }

        const uint8_t *rf = pe_runtime_function(m, pc);
        uint8_t cur[CTX_SIZE];
        memcpy(cur, work, CTX_SIZE); /* this frame's registers, before it is unwound */

        Frame f;
        if (!step_frame(work, pc, m, &f)) break;

        if (target && f.est == target) {
            /* Windows calls the target frame's handler once more with EXCEPTION_TARGET_UNWIND.
             * The C++ personality then writes the selector (rdx) into the context record.
             * Landing uses that record for the volatile registers. */
            uint8_t tctx[CTX_SIZE];
            memcpy(tctx, orig, CTX_SIZE);
            if (f.handler && (f.flags & UNW_FLAG_EHANDLER)) {
                uint8_t dc[80];
                uint32_t saved_flags;
                memcpy(&saved_flags, er + 4, 4);
                uint32_t tflags = EXCEPTION_UNWINDING | EXCEPTION_TARGET_UNWIND;
                memcpy(er + 4, &tflags, 4);
                fill_dispatch(dc, pc, m, rf, &f, target_ip, tctx);
                xbemu_call_w64((void *)(uintptr_t)f.handler, (uint64_t)(uintptr_t)er, f.est,
                               (uint64_t)(uintptr_t)tctx, (uint64_t)(uintptr_t)dc);
                memcpy(er + 4, &saved_flags, 4);
            }
            land(cur, tctx, target_ip, retval);
        }
        if (f.handler && (f.flags & UNW_FLAG_UHANDLER)) {
            uint8_t dc[80];
            fill_dispatch(dc, pc, m, rf, &f, target_ip, ctxcopy);
            xbemu_call_w64((void *)(uintptr_t)f.handler, (uint64_t)(uintptr_t)er, f.est,
                           (uint64_t)(uintptr_t)ctxcopy, (uint64_t)(uintptr_t)dc);
        }
    }
    if (!target && target_ip) land(work, orig, target_ip, retval);
}

/* Build a CONTEXT describing the guest state at the call into a host function. */
static void ctx_from_w64(const W64Ctx *c, uint8_t *ctx) {
    memset(ctx, 0, CTX_SIZE);
    cset(ctx, CTX_REG(REG_RCX), c->r[0]);
    cset(ctx, CTX_REG(REG_RDX), c->r[1]);
    cset(ctx, CTX_REG(REG_R8), c->r[2]);
    cset(ctx, CTX_REG(REG_R9), c->r[3]);
    cset(ctx, CTX_RSP, c->rsp);
    cset(ctx, CTX_RBP, c->rbp);
    cset(ctx, CTX_REG(REG_RBX), c->rbx);
    cset(ctx, CTX_REG(REG_RSI), c->rsi);
    cset(ctx, CTX_REG(REG_RDI), c->rdi);
    cset(ctx, CTX_REG(REG_R12), c->r12);
    cset(ctx, CTX_REG(REG_R12 + 1), c->r13);
    cset(ctx, CTX_REG(REG_R12 + 2), c->r14);
    cset(ctx, CTX_REG(REG_R15), c->r15);
    cset(ctx, CTX_RIP, c->rip);
    memcpy(ctx + CTX_XMM0, c->xmm, 4 * 16);
    memcpy(ctx + CTX_XMM0 + 6 * 16, c->xmm_nv, 10 * 16);
}

/* void RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args) */
static uint64_t seh_RaiseException(W64Ctx *c) {
    uint32_t code = (uint32_t)w64_arg(c, 0);
    uint32_t flags = (uint32_t)w64_arg(c, 1);
    uint32_t nargs = (uint32_t)w64_arg(c, 2);
    const uint64_t *args = (const uint64_t *)(uintptr_t)w64_arg(c, 3);

    uint8_t orig[CTX_SIZE], er[152];
    ctx_from_w64(c, orig);
    /* The C++ personality raises EXCEPTION_GCC_FORCED from inside its handler to start the
     * landing. Its arguments are (exception object, target frame, landing ip) at 0..2.
     * The outer dispatch's context record is still live, so unwind to the target now. */
    if (code == EXCEPTION_GCC_FORCED && nargs >= 4 && args && active_ctx) {
        /* The return value of the landing is the exception object, args[0]. */
        unwind_to(args[1], args[2], active_er, args[0], active_ctx);
        return 0; /* not reached when the landing succeeds */
    }
    fill_record(er, code, flags, c->rip, args ? nargs : 0, args);
    dispatch_exception(orig, er);
    return 0; /* returns only if no handler took the exception */
}

/* void RtlCaptureContext(PCONTEXT) */
static uint64_t seh_RtlCaptureContext(W64Ctx *c) {
    uint8_t *ctx = (uint8_t *)(uintptr_t)w64_arg(c, 0);
    if (ctx) ctx_from_w64(c, ctx);
    return 0;
}

/* PRUNTIME_FUNCTION RtlLookupFunctionEntry(DWORD64 pc, PDWORD64 imagebase, PUNWIND_HISTORY_TABLE) */
static uint64_t seh_RtlLookupFunctionEntry(W64Ctx *c) {
    uint64_t pc = w64_arg(c, 0);
    uint64_t *ibase = (uint64_t *)(uintptr_t)w64_arg(c, 1);
    PeModule *m = pe_find_module_by_addr((const void *)(uintptr_t)pc);
    if (!m) return 0;
    if (ibase) *ibase = (uint64_t)(uintptr_t)m->base;
    return (uint64_t)(uintptr_t)pe_runtime_function(m, pc);
}

/* PEXCEPTION_ROUTINE RtlVirtualUnwind(DWORD type, DWORD64 imagebase, DWORD64 pc,
 *     PRUNTIME_FUNCTION rf, PCONTEXT ctx, PVOID *handler_data, PDWORD64 est, PKNONVOLATILE_CONTEXT_POINTERS) */
static uint64_t seh_RtlVirtualUnwind(W64Ctx *c) {
    uint64_t pc = w64_arg(c, 2);
    const uint8_t *rf = (const uint8_t *)(uintptr_t)w64_arg(c, 3);
    uint8_t *ctx = (uint8_t *)(uintptr_t)w64_arg(c, 4);
    void **hdata_out = (void **)(uintptr_t)w64_arg(c, 5);
    uint64_t *est_out = (uint64_t *)(uintptr_t)w64_arg(c, 6);

    PeModule *m = pe_find_module_by_addr((const void *)(uintptr_t)pc);
    if (!m || !rf || !ctx) return 0;
    Frame f;
    if (!virtual_unwind(m, ctx, pc, rf, &f)) return 0;
    if (hdata_out) *hdata_out = (void *)(uintptr_t)f.hdata;
    if (est_out) *est_out = f.est;
    return f.handler;
}

/* VOID RtlUnwindEx(PVOID target, PVOID target_ip, PEXCEPTION_RECORD, PVOID retval,
 *                  PCONTEXT orig, PUNWIND_HISTORY_TABLE) */
static uint64_t seh_RtlUnwindEx(W64Ctx *c) {
    uint64_t target = w64_arg(c, 0);
    uint64_t target_ip = w64_arg(c, 1);
    uint8_t *er = (uint8_t *)(uintptr_t)w64_arg(c, 2);
    uint64_t retval = w64_arg(c, 3);
    const uint8_t *orig = (const uint8_t *)(uintptr_t)w64_arg(c, 4);
    if (!orig || !er) return 0;
    unwind_to(target, target_ip, er, retval, orig);
    return 0;
}

const HostExport kernel32_seh_exports[] = {
    { "RaiseException",          seh_RaiseException          },
    { "RtlCaptureContext",       seh_RtlCaptureContext       },
    { "RtlLookupFunctionEntry",  seh_RtlLookupFunctionEntry  },
    { "RtlVirtualUnwind",        seh_RtlVirtualUnwind        },
    { "RtlUnwindEx",             seh_RtlUnwindEx             },
    { NULL, NULL },
};

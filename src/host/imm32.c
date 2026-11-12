/*
 * imm32.dll shims: no input method. Composition (IME) is not implemented, and there is no
 * input-method server to test it against. Programs get the answer a Windows machine gives
 * when no IME is installed: no input context. They then take their direct-key path.
 */
#define _DEFAULT_SOURCE
#include "kernel32_impl.h"

#include <stdint.h>

/* HIMC ImmGetContext(HWND): none. */
static uint64_t imm_GetContext(W64Ctx *c) {
    (void)c;
    return 0;
}

/* BOOL ImmReleaseContext(HWND, HIMC) */
static uint64_t imm_ReleaseContext(W64Ctx *c) {
    (void)c;
    return 1;
}

/* BOOL ImmSetCompositionWindow / ImmSetCandidateWindow / ImmAssociateContext: nothing to set. */
static uint64_t imm_Fail(W64Ctx *c) {
    (void)c;
    return 0;
}

/* LONG ImmGetCompositionStringW(HIMC, DWORD index, LPVOID buf, DWORD len): no composition. */
static uint64_t imm_GetCompositionString(W64Ctx *c) {
    (void)c;
    return 0;
}

const HostExport imm32_exports[] = {
    { "ImmGetContext",              imm_GetContext              },
    { "ImmReleaseContext",          imm_ReleaseContext          },
    { "ImmSetCompositionWindow",    imm_Fail                    },
    { "ImmSetCandidateWindow",      imm_Fail                    },
    { "ImmAssociateContext",        imm_Fail                    },
    { "ImmGetCompositionStringW",   imm_GetCompositionString    },
    { NULL, NULL },
};

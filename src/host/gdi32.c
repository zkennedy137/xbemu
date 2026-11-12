/*
 * gdi32.dll shims. The D3DKMT calls are the Windows display driver model's kernel
 * interface. Linux has no equivalent, so they report STATUS_NOT_SUPPORTED. Adapter
 * information comes from Vulkan instead (see dxgi in DXVK). Device contexts are not
 * implemented either.
 */
#define _DEFAULT_SOURCE
#include "kernel32_impl.h"

#include <stdint.h>

#define STATUS_SUCCESS       0x00000000u
#define STATUS_NOT_SUPPORTED 0xC00000BBu

static uint64_t kmt_not_supported(W64Ctx *c) {
    (void)c;
    return STATUS_NOT_SUPPORTED;
}

static uint64_t kmt_success(W64Ctx *c) {
    (void)c;
    return STATUS_SUCCESS;
}

/* Device contexts: none can be created. */
static uint64_t gdi_null(W64Ctx *c) {
    (void)c;
    return 0;
}

const HostExport gdi32_exports[] = {
    { "D3DKMTCloseAdapter",                   kmt_success       },
    { "D3DKMTCreateDevice",                   kmt_not_supported },
    { "D3DKMTCreateDCFromMemory",             kmt_not_supported },
    { "D3DKMTCreateKeyedMutex2",              kmt_not_supported },
    { "D3DKMTDestroyAllocation",              kmt_not_supported },
    { "D3DKMTDestroyDCFromMemory",            kmt_not_supported },
    { "D3DKMTDestroyDevice",                  kmt_not_supported },
    { "D3DKMTDestroyKeyedMutex",              kmt_not_supported },
    { "D3DKMTDestroySynchronizationObject",   kmt_not_supported },
    { "D3DKMTEscape",                         kmt_not_supported },
    { "D3DKMTOpenAdapterFromLuid",            kmt_not_supported },
    { "D3DKMTOpenKeyedMutex",                 kmt_not_supported },
    { "D3DKMTOpenResource2",                  kmt_not_supported },
    { "D3DKMTOpenResourceFromNtHandle",       kmt_not_supported },
    { "D3DKMTOpenSynchronizationObject",      kmt_not_supported },
    { "D3DKMTOpenSyncObjectFromNtHandle",     kmt_not_supported },
    { "D3DKMTQueryResourceInfo",              kmt_not_supported },
    { "D3DKMTQueryResourceInfoFromNtHandle",  kmt_not_supported },
    { "D3DKMTShareObjects",                   kmt_not_supported },
    { "CreateCompatibleDC",                   gdi_null          },
    { NULL, NULL },
};

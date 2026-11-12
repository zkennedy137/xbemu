/*
 * vulkan-1.dll shim: forwards Vulkan calls from guest code to the host Vulkan loader
 * (libvulkan.so.1).
 *
 * Vulkan uses the same integer/pointer arguments on both ABIs, so a call forwards
 * by passing the Win64 argument slots (rcx, rdx, r8, r9, then stack) as the first
 * ten SysV integer arguments. Pointers and handles are host pointers, so they need
 * no translation.
 *
 * Limitation: Vulkan functions that take float arguments (for example
 * vkCmdSetLineWidth) will receive the wrong value. Their float is not forwarded yet.
 */
#define _GNU_SOURCE
#include "hostlib.h"

#include <dlfcn.h>
#include <stdlib.h>
#include "user32_impl.h"
#include "xdisplay.h"
#include <stdio.h>
#include <string.h>

#define VK_STUB_CACHE 1024

static void *libvk;
static void *host_gipa;  /* vkGetInstanceProcAddr on the host */
static void *host_gdpa;  /* vkGetDeviceProcAddr on the host */
static int vk_tried;

static int vk_init(void) {
    if (!vk_tried) {
        vk_tried = 1;
        libvk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!libvk) {
            fprintf(stderr, "[xbemu] cannot load libvulkan.so.1: %s\n", dlerror());
            return 0;
        }
        host_gipa = dlsym(libvk, "vkGetInstanceProcAddr");
        host_gdpa = dlsym(libvk, "vkGetDeviceProcAddr");
    }
    return libvk != NULL;
}

/* A forwarded function: the host pointer, and which Win64 argument positions are floats. */
typedef struct VkFwd {
    void *fn;
    uint32_t fmask;   /* bit p set: argument p is a float (passed in xmm p on Win64) */
} VkFwd;

/* Vulkan functions with float parameters, by Win64 argument position (generated table). */
#include "vulkan_floats.inc"

static uint32_t float_mask_for(const char *name) {
    for (size_t i = 0; i < sizeof vk_float_table / sizeof vk_float_table[0]; i++)
        if (strcmp(vk_float_table[i].name, name) == 0) return vk_float_table[i].mask;
    return 0;
}

/*
 * Handler for every forwarded function. Integer and pointer arguments go to the host's
 * integer registers in order, and float arguments go to its xmm registers in order.
 * Up to ten Win64 argument positions are forwarded.
 */
static uint64_t vk_forward(W64Ctx *c) {
    typedef uint64_t (*HostFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                               double, double, double, double, double, double, double, double,
                               uint64_t, uint64_t, uint64_t, uint64_t);
    VkFwd *f = w64_slot_user(c);
    if (!f || !f->fn) return 0;

    uint64_t iv[10] = { 0 };
    double dv[8] = { 0 };
    unsigned ni = 0, nf = 0;
    for (unsigned p = 0; p < 10; p++) {
        if (f->fmask & (1u << p)) {
            if (nf < 8) memcpy(&dv[nf++], &c->xmm[p & 3][0], 8);
        } else {
            iv[ni++] = w64_arg(c, p);
        }
    }
    static int trace = -1;
    if (trace < 0) trace = getenv("XBEMU_TRACE_VK") != NULL;
    if (trace && f->fmask) {
        for (unsigned p = 0; p < 10; p++)
            if (f->fmask & (1u << p)) {
                float v;
                memcpy(&v, &c->xmm[p & 3][0], 4);
                fprintf(stderr, "[vk] float arg %u = %g\n", p, v);
            }
    }
    return ((HostFn)f->fn)(iv[0], iv[1], iv[2], iv[3], iv[4], iv[5],
                           dv[0], dv[1], dv[2], dv[3], dv[4], dv[5], dv[6], dv[7],
                           iv[6], iv[7], iv[8], iv[9]);
}

/* ---- Win32 WSI extensions ----
 * DXVK requests VK_KHR_win32_surface, which the Linux loader does not provide. Device
 * creation does not need a window surface, so the extension is removed from create
 * requests. Win32 surface entry points return "not present" until a real surface
 * layer (X11/Wayland) is added. */

#define VK_KHR_WIN32_SURFACE_NAME "VK_KHR_win32_surface"
#define VK_KHR_XCB_SURFACE_NAME   "VK_KHR_xcb_surface"

#define VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR   1000005000
#define VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR 1000009000

typedef uint64_t (*HostCreateFn)(const void *info, const void *alloc, void *out);

static void *host_sym(const char *name) {
    return vk_init() ? dlsym(libvk, name) : NULL;
}

/* Call a host create function with the Win32 surface extension removed from the
 * extension list at [count_off, names_off] in the create-info struct. */
static uint64_t create_without_win32(void *host_fn, const void *info, size_t size,
                                     size_t count_off, size_t names_off,
                                     const void *alloc, void *out, const char *replacement) {
    uint8_t copy[128];
    if (size > sizeof copy) return (uint64_t)(uint32_t)-3; /* VK_ERROR_INITIALIZATION_FAILED */
    memcpy(copy, info, size);

    uint32_t count;
    const char *const *names;
    memcpy(&count, copy + count_off, 4);
    memcpy(&names, copy + names_off, 8);

    const char **kept = malloc(sizeof(char *) * (count ? count : 1));
    if (!kept) return (uint64_t)(uint32_t)-2; /* VK_ERROR_OUT_OF_HOST_MEMORY */
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(names[i], VK_KHR_WIN32_SURFACE_NAME) == 0) {
            if (replacement) kept[n++] = replacement;
        } else {
            kept[n++] = names[i];
        }
    }

    memcpy(copy + count_off, &n, 4);
    const void *kp = kept;
    memcpy(copy + names_off, &kp, 8);

    uint64_t rc = ((HostCreateFn)host_fn)(copy, alloc, out);
    free(kept);
    return rc;
}

/* Host instance, kept for the instance-level functions that need it (surface support). */
static void *g_instance;

/* VkResult vkCreateInstance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *)
 * VK_KHR_win32_surface is replaced by VK_KHR_xcb_surface, the X11 surface the host provides. */
static uint64_t vk_CreateInstance(W64Ctx *c) {
    void *fn = host_sym("vkCreateInstance");
    if (!fn) return (uint64_t)(uint32_t)-3;
    void **out = (void **)(uintptr_t)w64_arg(c, 2);
    uint64_t rc = create_without_win32(fn, (const void *)(uintptr_t)w64_arg(c, 0), 64,
                                       48 /* enabledExtensionCount */, 56 /* ppEnabledExtensionNames */,
                                       (const void *)(uintptr_t)w64_arg(c, 1), out, VK_KHR_XCB_SURFACE_NAME);
    if ((int32_t)rc == 0 && out) g_instance = *out;
    return rc;
}

/* VkResult vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *) */
static uint64_t vk_CreateDevice(W64Ctx *c) {
    void *fn = host_sym("vkCreateDevice");
    if (!fn) return (uint64_t)(uint32_t)-3;
    /* The physical device is the first argument, so the host call takes it as well. */
    typedef uint64_t (*HostDeviceFn)(void *, const void *, const void *, void *);
    uint8_t copy[128];
    const void *info = (const void *)(uintptr_t)w64_arg(c, 1);
    memcpy(copy, info, 72);
    uint32_t count;
    const char *const *names;
    memcpy(&count, copy + 48, 4);
    memcpy(&names, copy + 56, 8);
    const char **kept = malloc(sizeof(char *) * (count ? count : 1));
    if (!kept) return (uint64_t)(uint32_t)-2;
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++)
        if (strcmp(names[i], VK_KHR_WIN32_SURFACE_NAME) != 0) kept[n++] = names[i];
    memcpy(copy + 48, &n, 4);
    const void *kp = kept;
    memcpy(copy + 56, &kp, 8);
    uint64_t rc = ((HostDeviceFn)fn)((void *)(uintptr_t)w64_arg(c, 0), copy,
                                     (const void *)(uintptr_t)w64_arg(c, 2),
                                     (void *)(uintptr_t)w64_arg(c, 3));
    free(kept);
    return rc;
}

/* VkResult vkCreateWin32SurfaceKHR(VkInstance, const VkWin32SurfaceCreateInfoKHR *,
 *                                  const VkAllocationCallbacks *, VkSurfaceKHR *)
 * The HWND's X11 window becomes an xcb surface on the host. */
static uint64_t vk_CreateWin32Surface(W64Ctx *c) {
    const uint8_t *info = (const uint8_t *)(uintptr_t)w64_arg(c, 1);
    if (!info) return (uint64_t)(uint32_t)-2;
    uint64_t hwnd;
    memcpy(&hwnd, info + 32, 8); /* VkWin32SurfaceCreateInfoKHR.hwnd */

    xcb_window_t xw;
    xcb_connection_t *conn = xbemu_xcb_surface_connection();
    if (!conn || !xbemu_hwnd_to_xcb(hwnd, &xw)) return (uint64_t)(uint32_t)-1; /* VK_ERROR_UNKNOWN */

    /* VkXcbSurfaceCreateInfoKHR: sType, pNext, flags, connection, window */
    uint8_t xinfo[40] = { 0 };
    uint32_t stype = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    memcpy(xinfo + 0, &stype, 4);
    memcpy(xinfo + 24, &conn, 8);
    memcpy(xinfo + 32, &xw, 4);

    void *fn = host_gipa ? ((void *(*)(void *, const char *))host_gipa)(
                               (void *)(uintptr_t)w64_arg(c, 0), "vkCreateXcbSurfaceKHR") : NULL;
    if (!fn) return (uint64_t)(uint32_t)-7; /* VK_ERROR_EXTENSION_NOT_PRESENT */
    typedef uint64_t (*CreateXcb)(void *, const void *, const void *, void *);
    return ((CreateXcb)fn)((void *)(uintptr_t)w64_arg(c, 0), xinfo,
                           (const void *)(uintptr_t)w64_arg(c, 2), (void *)(uintptr_t)w64_arg(c, 3));
}

/* VkBool32 vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice, uint32 queueFamily)
 * Maps to the X11 query for the default screen's root visual. */
static uint64_t vk_Win32PresentationSupport(W64Ctx *c) {
    xcb_connection_t *conn = xbemu_xcb_surface_connection();
    xcb_screen_t *scr = xbemu_xcb_screen();
    if (!conn || !scr || !g_instance) return 0;
    void *fn = ((void *(*)(void *, const char *))host_gipa)(g_instance,
                                                           "vkGetPhysicalDeviceXcbPresentationSupportKHR");
    if (!fn) return 0;
    typedef uint32_t (*PresentSupport)(void *, uint32_t, void *, uint32_t);
    return ((PresentSupport)fn)((void *)(uintptr_t)w64_arg(c, 0), (uint32_t)w64_arg(c, 1),
                                conn, scr->root_visual);
}

/* Guest stub for a host Vulkan function. Cached by host pointer, so each function gets one stub. */
static void *forwarder_for(const char *name, void *host_fn) {
    static struct { void *host; void *stub; } cache[VK_STUB_CACHE];
    static size_t count;

    for (size_t i = 0; i < count; i++)
        if (cache[i].host == host_fn) return cache[i].stub;

    VkFwd *f = malloc(sizeof *f);
    if (!f) return NULL;
    f->fn = host_fn;
    f->fmask = float_mask_for(name);
    void *stub = w64_make_stub_user("vulkan-1.dll", name, vk_forward, f);
    if (stub && count < VK_STUB_CACHE) {
        cache[count].host = host_fn;
        cache[count].stub = stub;
        count++;
    }
    return stub;
}

static uint64_t vk_GetInstanceProcAddr(W64Ctx *c);
static uint64_t vk_GetDeviceProcAddr(W64Ctx *c);

/* Guest stubs for the wrapper functions. Created once, on first use. */
static void *wrapper_stub(const char *name, W64Fn fn) {
    static struct { const char *name; void *stub; } cache[8];
    static size_t count;
    for (size_t i = 0; i < count; i++)
        if (strcmp(cache[i].name, name) == 0) return cache[i].stub;
    void *stub = w64_make_stub_user("vulkan-1.dll", name, fn, NULL);
    if (stub && count < 8) { cache[count].name = name; cache[count].stub = stub; count++; }
    return stub;
}

/* PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance, const char *name) */
static uint64_t vk_GetInstanceProcAddr(W64Ctx *c) {
    void *instance = (void *)(uintptr_t)w64_arg(c, 0);
    const char *name = (const char *)(uintptr_t)w64_arg(c, 1);
    if (!name) return 0;
    if (strcmp(name, "vkCreateInstance") == 0) return (uint64_t)(uintptr_t)wrapper_stub(name, vk_CreateInstance);
    if (strcmp(name, "vkCreateDevice") == 0) return (uint64_t)(uintptr_t)wrapper_stub(name, vk_CreateDevice);
    /* These return function pointers, so they must be guest stubs too. Returning the
     * host trampolines here would hand guest code raw host pointers. */
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) return (uint64_t)(uintptr_t)wrapper_stub(name, vk_GetInstanceProcAddr);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return (uint64_t)(uintptr_t)wrapper_stub(name, vk_GetDeviceProcAddr);
    if (strcmp(name, "vkCreateWin32SurfaceKHR") == 0)
        return (uint64_t)(uintptr_t)wrapper_stub(name, vk_CreateWin32Surface);
    if (strcmp(name, "vkGetPhysicalDeviceWin32PresentationSupportKHR") == 0)
        return (uint64_t)(uintptr_t)wrapper_stub(name, vk_Win32PresentationSupport);
    if (!vk_init() || !host_gipa) return 0;
    void *host_fn = ((void *(*)(void *, const char *))host_gipa)(instance, name);
    if (!host_fn) return 0;
    return (uint64_t)(uintptr_t)forwarder_for(name, host_fn);
}

/* PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice, const char *name) */
static uint64_t vk_GetDeviceProcAddr(W64Ctx *c) {
    void *device = (void *)(uintptr_t)w64_arg(c, 0);
    const char *name = (const char *)(uintptr_t)w64_arg(c, 1);
    if (!vk_init() || !host_gdpa || !name) return 0;
    void *host_fn = ((void *(*)(void *, const char *))host_gdpa)(device, name);
    if (!host_fn) return 0;
    return (uint64_t)(uintptr_t)forwarder_for(name, host_fn);
}

const HostExport vulkan_exports[] = {
    { "vkGetInstanceProcAddr", vk_GetInstanceProcAddr },
    { "vkGetDeviceProcAddr",   vk_GetDeviceProcAddr   },
    { "vkCreateInstance",      vk_CreateInstance      },
    { "vkCreateDevice",        vk_CreateDevice        },
    { NULL, NULL },
};

/* Other Vulkan entry points: look them up in the host loader by name. */
void *vulkan_dynamic_resolve(const char *dll, const char *name) {
    (void)dll;
    if (!vk_init()) return NULL;
    if (strcmp(name, "vkCreateWin32SurfaceKHR") == 0)
        return wrapper_stub(name, vk_CreateWin32Surface);
    if (strcmp(name, "vkGetPhysicalDeviceWin32PresentationSupportKHR") == 0)
        return wrapper_stub(name, vk_Win32PresentationSupport);
    void *host_fn = dlsym(libvk, name);
    if (!host_fn) return NULL;
    return forwarder_for(name, host_fn);
}

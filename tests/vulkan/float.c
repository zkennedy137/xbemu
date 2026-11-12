/*
 * Win64 Vulkan float test: records vkCmdSetLineWidth and vkCmdSetDepthBounds (both take
 * floats) through vulkan-1.dll. With XBEMU_TRACE_VK set, the shim prints the float values
 * it forwards, and tests/vulkan/run_float_test.sh checks them.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/* The few Win32 declarations needed here. Windows headers would clash with the host's stdint. */
typedef void *HANDLE;
typedef void *HMODULE;
__declspec(dllimport) HANDLE GetStdHandle(unsigned long);
__declspec(dllimport) int WriteFile(HANDLE, const void *, unsigned long, unsigned long *, void *);
__declspec(dllimport) HMODULE LoadLibraryA(const char *);
__declspec(dllimport) void *GetProcAddress(HMODULE, const char *);
__declspec(dllimport) void ExitProcess(unsigned int) __attribute__((noreturn));

static void wr(const char *s) {
    unsigned long n = 0, w = 0;
    while (s[n]) n++;
    WriteFile(GetStdHandle((unsigned long)-11), s, n, &w, NULL);
}

typedef VkResult (VKAPI_PTR *PFN_create_instance)(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
typedef VkResult (VKAPI_PTR *PFN_enum_pd)(VkInstance, uint32_t *, VkPhysicalDevice *);
typedef void (VKAPI_PTR *PFN_qf)(VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *);
typedef VkResult (VKAPI_PTR *PFN_create_device)(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
typedef VkResult (VKAPI_PTR *PFN_create_pool)(VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *);
typedef VkResult (VKAPI_PTR *PFN_alloc_cb)(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
typedef VkResult (VKAPI_PTR *PFN_begin_cb)(VkCommandBuffer, const VkCommandBufferBeginInfo *);
typedef VkResult (VKAPI_PTR *PFN_end_cb)(VkCommandBuffer);
typedef void (VKAPI_PTR *PFN_set_line_width)(VkCommandBuffer, float);
typedef void (VKAPI_PTR *PFN_set_depth_bounds)(VkCommandBuffer, float, float);

void entry(void) {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { wr("no vulkan-1.dll\n"); ExitProcess(1); }
    PFN_create_instance create_instance = (PFN_create_instance)GetProcAddress(vk, "vkCreateInstance");
    PFN_enum_pd enum_pd = (PFN_enum_pd)GetProcAddress(vk, "vkEnumeratePhysicalDevices");
    PFN_qf qf = (PFN_qf)GetProcAddress(vk, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_create_device create_device = (PFN_create_device)GetProcAddress(vk, "vkCreateDevice");
    PFN_create_pool create_pool = (PFN_create_pool)GetProcAddress(vk, "vkCreateCommandPool");
    PFN_alloc_cb alloc_cb = (PFN_alloc_cb)GetProcAddress(vk, "vkAllocateCommandBuffers");
    PFN_begin_cb begin_cb = (PFN_begin_cb)GetProcAddress(vk, "vkBeginCommandBuffer");
    PFN_end_cb end_cb = (PFN_end_cb)GetProcAddress(vk, "vkEndCommandBuffer");
    PFN_set_line_width set_line_width = (PFN_set_line_width)GetProcAddress(vk, "vkCmdSetLineWidth");
    PFN_set_depth_bounds set_depth_bounds = (PFN_set_depth_bounds)GetProcAddress(vk, "vkCmdSetDepthBounds");
    if (!create_instance || !enum_pd || !qf || !create_device || !create_pool || !alloc_cb ||
        !begin_cb || !end_cb || !set_line_width || !set_depth_bounds) {
        wr("missing Vulkan entry point\n");
        ExitProcess(2);
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    VkInstance inst = NULL;
    if (create_instance(&ici, NULL, &inst) != VK_SUCCESS) { wr("vkCreateInstance failed\n"); ExitProcess(3); }

    uint32_t npd = 1;
    VkPhysicalDevice pd = NULL;
    enum_pd(inst, &npd, &pd);
    uint32_t nq = 0;
    qf(pd, &nq, NULL);
    VkQueueFamilyProperties qprops[16];
    if (nq > 16) nq = 16;
    qf(pd, &nq, qprops);
    uint32_t family = 0;
    for (uint32_t i = 0; i < nq; i++)
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev = NULL;
    if (create_device(pd, &dci, NULL, &dev) != VK_SUCCESS) { wr("vkCreateDevice failed\n"); ExitProcess(4); }

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = family;
    VkCommandPool pool = NULL;
    create_pool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cb = NULL;
    alloc_cb(dev, &cbi, &cb);

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin_cb(cb, &bi);
    set_line_width(cb, 2.5f);
    set_depth_bounds(cb, 0.25f, 0.75f);
    VkResult r = end_cb(cb);
    wr(r == VK_SUCCESS ? "float calls recorded\n" : "end command buffer failed\n");
    ExitProcess(r == VK_SUCCESS ? 0 : 5);
}

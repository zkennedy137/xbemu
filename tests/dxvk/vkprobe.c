/* Win64 probe: LoadLibrary("winevulkan.dll") then GetProcAddress("vkGetInstanceProcAddr"). */
#include <stdint.h>
typedef uint32_t DWORD;
__declspec(dllimport) void *GetStdHandle(DWORD);
__declspec(dllimport) int WriteFile(void *, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) void ExitProcess(unsigned int) __attribute__((noreturn));
__declspec(dllimport) void *LoadLibraryA(const char *);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
static void wr(const char *s) { DWORD n = 0, w = 0; while (s[n]) n++; WriteFile(GetStdHandle((DWORD)-11), s, n, &w, 0); }
static void whex(uint64_t v) {
    char b[20]; int i = 19; b[i] = 0;
    do { int d = (int)(v & 0xF); b[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; } while (v);
    b[--i] = 'x'; b[--i] = '0'; wr(&b[i]);
}
void entry(void) {
    void *h = LoadLibraryA("winevulkan.dll");
    wr("winevulkan handle = "); whex((uint64_t)(uintptr_t)h); wr("\n");
    void *p = h ? GetProcAddress(h, "vkGetInstanceProcAddr") : 0;
    wr("vkGetInstanceProcAddr = "); whex((uint64_t)(uintptr_t)p); wr("\n");
    ExitProcess(p ? 0 : 6);
}

/*
 * Win64 probe: load DXVK's d3d11.dll through LoadLibrary and look up D3D11CreateDevice.
 * Imports kernel32 only. Output shows where module loading stops.
 */
#include <stddef.h>
#include <stdint.h>

typedef uint32_t DWORD;
typedef void *HANDLE;
typedef int BOOL;

__declspec(dllimport) HANDLE GetStdHandle(DWORD);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) void ExitProcess(unsigned int) __attribute__((noreturn));
__declspec(dllimport) void *LoadLibraryA(const char *);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
__declspec(dllimport) DWORD GetLastError(void);

static void wr(const char *s) {
    DWORD n = 0;
    while (s[n]) n++;
    DWORD w = 0;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &w, NULL);
}

static void whex(uint64_t v) {
    char b[20];
    int i = 19;
    b[i] = 0;
    do { int d = (int)(v & 0xF); b[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; } while (v);
    b[--i] = 'x';
    b[--i] = '0';
    wr(&b[i]);
}

void entry(void) {
    void *d3d11 = LoadLibraryA("d3d11.dll");
    wr("LoadLibrary(d3d11.dll) = ");
    whex((uint64_t)(uintptr_t)d3d11);
    wr("\n");
    if (!d3d11) {
        wr("error ");
        whex(GetLastError());
        wr("\n");
        ExitProcess(3);
    }

    void *create = GetProcAddress(d3d11, "D3D11CreateDevice");
    wr("GetProcAddress(D3D11CreateDevice) = ");
    whex((uint64_t)(uintptr_t)create);
    wr("\n");
    if (!create) ExitProcess(4);

    /* HRESULT D3D11CreateDevice(adapter, driver, software, flags, levels, nlevels,
     *                           sdk_version, &device, &feature_level, &context) */
    typedef long (*PFN_D3D11CreateDevice)(void *, unsigned, void *, unsigned, const unsigned *,
                                          unsigned, unsigned, void **, unsigned *, void **);
    static const unsigned levels[] = { 0xb100 }; /* D3D_FEATURE_LEVEL_11_0 */
    void *device = 0, *context = 0;
    unsigned got_level = 0;
    long hr = ((PFN_D3D11CreateDevice)create)(0, 1 /* HARDWARE */, 0, 0, levels, 1, 7,
                                              &device, &got_level, &context);
    wr("D3D11CreateDevice hr = ");
    whex((uint64_t)(uint32_t)hr);
    wr("\n");
    if (device) {
        wr("device created, feature level ");
        whex(got_level);
        wr("\n");
    }
    ExitProcess(hr == 0 ? 0 : 5);
}

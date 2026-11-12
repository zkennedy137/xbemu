/*
 * Win64 DXGI probe: enumerate adapters and outputs, and list display modes for the
 * output. Prints adapter description, output device name and desktop rect, and mode count.
 */
#define COBJMACROS
#include <stdint.h>
#include <windows.h>
#include <dxgi.h>

static void wr(const char *s) {
    DWORD n = 0, w = 0;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &w, NULL);
}

static void wnum(int64_t v) {
    char b[24];
    int i = 23;
    b[i] = 0;
    uint64_t u = v < 0 ? (uint64_t)-v : (uint64_t)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) b[--i] = '-';
    wr(&b[i]);
}

static void wwide(const WCHAR *s) {
    char b[160];
    size_t i = 0;
    for (; s[i] && i + 1 < sizeof b; i++) b[i] = s[i] < 0x80 ? (char)s[i] : '?';
    b[i] = 0;
    wr(b);
}

typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void **);

void entry(void) {
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    PFN_CreateDXGIFactory1 create = dxgi ? (PFN_CreateDXGIFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    if (!create) ExitProcess(1);

    IDXGIFactory1 *factory = NULL;
    if (FAILED(create(&IID_IDXGIFactory1, (void **)&factory))) ExitProcess(2);

    IDXGIAdapter1 *adapter = NULL;
    for (UINT a = 0; IDXGIFactory1_EnumAdapters1(factory, a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter1_GetDesc1(adapter, &desc);
        wr("adapter "); wnum(a); wr(": "); wwide(desc.Description); wr("\n");

        IDXGIOutput *output = NULL;
        for (UINT o = 0; IDXGIAdapter1_EnumOutputs(adapter, o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            DXGI_OUTPUT_DESC od;
            IDXGIOutput_GetDesc(output, &od);
            wr("  output "); wnum(o); wr(": "); wwide(od.DeviceName);
            wr(" rect "); wnum(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
            wr("x"); wnum(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
            wr(" primary-attached "); wnum(od.AttachedToDesktop); wr("\n");

            UINT modes = 0;
            IDXGIOutput_GetDisplayModeList(output, DXGI_FORMAT_R8G8B8A8_UNORM, 0, &modes, NULL);
            wr("  modes: "); wnum(modes); wr("\n");
            IDXGIOutput_Release(output);
        }
        IDXGIAdapter1_Release(adapter);
    }
    ExitProcess(0);
}

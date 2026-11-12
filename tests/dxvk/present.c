/*
 * Win64 test: D3D11 clear and present into a Win32 window.
 * Built against the MinGW headers, so struct layouts and COM vtables come from the SDK.
 * Clears the back buffer to (0.2, 0.4, 0.8), presents, and keeps the window up for a few
 * seconds so a screenshot can check the color.
 */

/* Sync interval for Present: 0 = immediate, 1 = wait for vblank (FIFO). */
#ifndef PRESENT_SYNC
#define PRESENT_SYNC 0
#endif
#define COBJMACROS
#include <stdint.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                const D3D_FEATURE_LEVEL *, UINT, UINT,
                                                ID3D11Device **, D3D_FEATURE_LEVEL *,
                                                ID3D11DeviceContext **);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void **);

static void wr(const char *s) {
    DWORD n = 0, w = 0;
    while (s[n]) n++;
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

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static int fail(const char *what, HRESULT hr) {
    wr(what);
    whex((uint64_t)(uint32_t)hr);
    wr("\n");
    ExitProcess(1);
    return 1;
}

void entry(void) {
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"xbemu_present";
    if (!RegisterClassExW(&wc)) fail("RegisterClassExW failed\n", 0);

    HWND hwnd = CreateWindowExW(0, L"xbemu_present", L"xbemu present test",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, NULL, NULL, NULL);
    if (!hwnd) fail("CreateWindowExW failed\n", 0);
    wr("window created\n");

    HMODULE d3d = LoadLibraryA("d3d11.dll");
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    if (!d3d || !dxgi) fail("LoadLibrary d3d11/dxgi failed\n", 0);
    PFN_D3D11CreateDevice create = (PFN_D3D11CreateDevice)GetProcAddress(d3d, "D3D11CreateDevice");
    PFN_CreateDXGIFactory1 create_factory = (PFN_CreateDXGIFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (!create || !create_factory) fail("GetProcAddress failed\n", 0);

    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL level = 0;
    HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 1, D3D11_SDK_VERSION,
                        &dev, &level, &ctx);
    if (FAILED(hr)) fail("D3D11CreateDevice hr = ", hr);
    wr("device created\n");

    IDXGIFactory1 *factory = NULL;
    hr = create_factory(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr)) fail("CreateDXGIFactory1 hr = ", hr);

    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 480;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain *swap = NULL;
    hr = IDXGIFactory1_CreateSwapChain(factory, (IUnknown *)dev, &sd, &swap);
    if (FAILED(hr)) fail("CreateSwapChain hr = ", hr);
    wr("swapchain created\n");

    ID3D11Texture2D *back = NULL;
    hr = IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back);
    if (FAILED(hr)) fail("GetBuffer hr = ", hr);

    ID3D11RenderTargetView *rtv = NULL;
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);
    if (FAILED(hr)) fail("CreateRenderTargetView hr = ", hr);

    const FLOAT color[4] = { 0.2f, 0.4f, 0.8f, 1.0f };
    for (int frame = 0; frame < 60; frame++) {
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, color);
        hr = IDXGISwapChain_Present(swap, PRESENT_SYNC, 0);
        if (FAILED(hr)) fail("Present hr = ", hr);
        Sleep(16);
    }
    wr("presented\n");

    /* Keep the window up for a screenshot, pumping messages as a real app would. */
    ULONGLONG start = GetTickCount64();
    MSG msg;
    int quit = 0;
    while (!quit && GetTickCount64() - start < 4000) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = 1; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
    wr("done\n");
    ExitProcess(0);
}

/*
 * Win64 draw test: a red triangle through D3D11 (vertex buffer, input layout, shaders,
 * draw call), presented to a window. Shader bytecode comes from make_shaders.py.
 */
#define COBJMACROS
#include <stdint.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "shaders.h"

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

static void fail(const char *what, HRESULT hr) {
    wr(what);
    whex((uint64_t)(uint32_t)hr);
    wr("\n");
    ExitProcess(1);
}

#define CHECK(expr, what) do { HRESULT _hr = (expr); if (FAILED(_hr)) fail(what, _hr); } while (0)

typedef struct { float x, y, z, w; } Vertex;

void entry(void) {
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"xbemu_triangle";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"xbemu_triangle", L"xbemu triangle test",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                640, 480, NULL, NULL, NULL, NULL);
    if (!hwnd) ExitProcess(1);

    HMODULE d3d = LoadLibraryA("d3d11.dll");
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    PFN_D3D11CreateDevice create = (PFN_D3D11CreateDevice)GetProcAddress(d3d, "D3D11CreateDevice");
    PFN_CreateDXGIFactory1 create_factory = (PFN_CreateDXGIFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (!create || !create_factory) ExitProcess(1);

    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL level = 0;
    CHECK(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 1, D3D11_SDK_VERSION, &dev, &level, &ctx),
          "D3D11CreateDevice hr = ");

    IDXGIFactory1 *factory = NULL;
    CHECK(create_factory(&IID_IDXGIFactory1, (void **)&factory), "CreateDXGIFactory1 hr = ");

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
    CHECK(IDXGIFactory1_CreateSwapChain(factory, (IUnknown *)dev, &sd, &swap), "CreateSwapChain hr = ");

    ID3D11Texture2D *back = NULL;
    CHECK(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back), "GetBuffer hr = ");
    ID3D11RenderTargetView *rtv = NULL;
    CHECK(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv), "CreateRTV hr = ");

    /* Shaders and the input layout that matches the vertex shader's POSITION input. */
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    CHECK(ID3D11Device_CreateVertexShader(dev, g_vs_dxbc, sizeof g_vs_dxbc, NULL, &vs), "CreateVertexShader hr = ");
    CHECK(ID3D11Device_CreatePixelShader(dev, g_ps_dxbc, sizeof g_ps_dxbc, NULL, &ps), "CreatePixelShader hr = ");

    D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *layout = NULL;
    CHECK(ID3D11Device_CreateInputLayout(dev, layout_desc, 1, g_vs_dxbc, sizeof g_vs_dxbc, &layout),
          "CreateInputLayout hr = ");

    /* Triangle in clip space: top centre, bottom right, bottom left. */
    static const Vertex verts[3] = {
        { 0.0f, 0.5f, 0.0f, 1.0f },
        { 0.5f, -0.5f, 0.0f, 1.0f },
        { -0.5f, -0.5f, 0.0f, 1.0f },
    };
    D3D11_BUFFER_DESC bd = { 0 };
    bd.ByteWidth = sizeof verts;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = { verts, 0, 0 };
    ID3D11Buffer *vb = NULL;
    CHECK(ID3D11Device_CreateBuffer(dev, &bd, &init, &vb), "CreateBuffer hr = ");

    /* Pipeline state and draw. */
    const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    UINT stride = sizeof(Vertex), offset = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    CHECK(IDXGISwapChain_Present(swap, 0, 0), "Present hr = ");
    wr("drawn\n");

    /* Keep the window up for a screenshot, pumping messages. */
    ULONGLONG start = GetTickCount64();
    MSG msg;
    while (GetTickCount64() - start < 4000) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
done:
    wr("done\n");
    ExitProcess(0);
}

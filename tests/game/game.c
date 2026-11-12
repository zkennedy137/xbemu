/*
 * Homebrew test game (original code, no third-party assets). It exercises the emulator's
 * main paths in one program:
 *   - window and message loop, with keyboard state from WM_KEYDOWN / WM_KEYUP
 *   - D3D11 rendering with shaders, a vertex buffer updated every frame, and vsync (FIFO)
 *   - XAudio2: a short beep on space (skipped if no audio device is available)
 * Controls: arrow keys move the triangle, space beeps, Escape quits. The game also
 * quits after RUN_MS, so it can run unattended. It prints "frames N" and "done" on exit.
 */
#define COBJMACROS
#include <stdint.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <xaudio2.h>
#include "shaders.h"

typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                const D3D_FEATURE_LEVEL *, UINT, UINT,
                                                ID3D11Device **, D3D_FEATURE_LEVEL *,
                                                ID3D11DeviceContext **);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void **);
typedef HRESULT (WINAPI *PFN_XAudio2Create)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);

#define WIDTH 640
#define HEIGHT 480
#define RUN_MS 6000
#define SPEED 0.8f          /* clip-space units per second */
#define HALF 0.08f          /* triangle half-size */
#define RATE 48000
#define BEEP_FRAMES 5760    /* 120 ms */

static volatile LONG key_left, key_right, key_up, key_down, key_esc;
static volatile LONG beep_request;

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

static void whex(uint64_t v) {
    char b[20];
    int i = 19;
    b[i] = 0;
    do { int d = (int)(v & 0xF); b[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; } while (v);
    b[--i] = 'x';
    b[--i] = '0';
    wr(&b[i]);
}

static void fail(const char *what, HRESULT hr) {
    wr(what);
    whex((uint64_t)(uint32_t)hr);
    wr("\n");
    ExitProcess(1);
}

#define CHECK(expr, what) do { HRESULT _hr = (expr); if (FAILED(_hr)) fail(what, _hr); } while (0)

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)l;
    switch (m) {
    case WM_KEYDOWN:
    case WM_KEYUP: {
        LONG down = (m == WM_KEYDOWN);
        switch (w) {
        case VK_LEFT:  key_left = down; break;
        case VK_RIGHT: key_right = down; break;
        case VK_UP:    key_up = down; break;
        case VK_DOWN:  key_down = down; break;
        case VK_ESCAPE: key_esc = down; break;
        case VK_SPACE:
            if (down) InterlockedExchange(&beep_request, 1);
            break;
        default: break;
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(h, m, w, l);
    }
}

/* Square beep at 880 Hz with a short fade, used for every space press. */
static int16_t beep_pcm[BEEP_FRAMES * 2];

static void make_beep(void) {
    uint32_t phase = 0;
    const uint32_t step = (uint32_t)(880.0 * 4294967296.0 / RATE);
    for (int i = 0; i < BEEP_FRAMES; i++) {
        int16_t v = (phase & 0x80000000u) ? 6000 : -6000;
        int32_t fade = (BEEP_FRAMES - i) * 1000 / BEEP_FRAMES;   /* fades to 0 */
        v = (int16_t)((v * fade) / 1000);
        beep_pcm[i * 2] = v;
        beep_pcm[i * 2 + 1] = v;
        phase += step;
    }
}

typedef struct { float x, y, z, w; } Vertex;

void entry(void) {
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"xbemu_game";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"xbemu_game", L"xbemu test game (arrows move, space beeps, Esc quits)",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                WIDTH, HEIGHT, NULL, NULL, NULL, NULL);
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
    sd.BufferDesc.Width = WIDTH;
    sd.BufferDesc.Height = HEIGHT;
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

    /* One vertex buffer, rewritten every frame with the triangle's current position. */
    Vertex verts[3];
    D3D11_BUFFER_DESC bd = { 0 };
    bd.ByteWidth = sizeof verts;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    ID3D11Buffer *vb = NULL;
    CHECK(ID3D11Device_CreateBuffer(dev, &bd, NULL, &vb), "CreateBuffer hr = ");

    /* Audio is optional: the game runs silently if XAudio2 can't start. */
    IXAudio2 *xa = NULL;
    IXAudio2MasteringVoice *mv = NULL;
    IXAudio2SourceVoice *sv = NULL;
    make_beep();
    HMODULE xa_dll = LoadLibraryA("xaudio2_9.dll");
    PFN_XAudio2Create xa_create = xa_dll ? (PFN_XAudio2Create)GetProcAddress(xa_dll, "XAudio2Create") : NULL;
    WAVEFORMATEX wfx = { WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0 };
    if (xa_create && SUCCEEDED(xa_create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) &&
        SUCCEEDED(IXAudio2_CreateMasteringVoice(xa, &mv, 2, RATE, 0, NULL, NULL, AudioCategory_GameEffects)) &&
        SUCCEEDED(IXAudio2_CreateSourceVoice(xa, &sv, &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL))) {
        IXAudio2SourceVoice_Start(sv, 0, XAUDIO2_COMMIT_NOW);
        wr("audio on\n");
    } else {
        sv = NULL;
        wr("audio off\n");
    }

    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (FLOAT)WIDTH, (FLOAT)HEIGHT, 0.0f, 1.0f };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    UINT stride = sizeof(Vertex), offset = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);

    const FLOAT background[4] = { 0.05f, 0.05f, 0.2f, 1.0f };
    float px = 0.0f, py = 0.0f;
    ULONGLONG start = GetTickCount64(), last = start;
    ULONGLONG frames = 0, beeps = 0;
    int quit = 0;
    wr("ready\n");

    while (!quit) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = 1; break; }
            DispatchMessageW(&msg);
        }
        if (quit) break;

        ULONGLONG now = GetTickCount64();
        float dt = (float)(now - last) / 1000.0f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;   /* a stall should not teleport the triangle */

        if (key_left) px -= SPEED * dt;
        if (key_right) px += SPEED * dt;
        if (key_up) py += SPEED * dt;
        if (key_down) py -= SPEED * dt;
        if (px < -0.9f) px = -0.9f;
        if (px > 0.9f) px = 0.9f;
        if (py < -0.9f) py = -0.9f;
        if (py > 0.9f) py = 0.9f;

        if (InterlockedExchange(&beep_request, 0) && sv) {
            XAUDIO2_BUFFER buf = { 0 };
            buf.AudioBytes = BEEP_FRAMES * 4;
            buf.pAudioData = (const BYTE *)beep_pcm;
            buf.Flags = XAUDIO2_END_OF_STREAM;
            if (SUCCEEDED(IXAudio2SourceVoice_SubmitSourceBuffer(sv, &buf, NULL))) beeps++;
        }

        verts[0] = (Vertex){ px, py + HALF, 0.0f, 1.0f };
        verts[1] = (Vertex){ px + HALF, py - HALF, 0.0f, 1.0f };
        verts[2] = (Vertex){ px - HALF, py - HALF, 0.0f, 1.0f };
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)vb, 0, NULL, verts, 0, 0);

        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, background);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        CHECK(IDXGISwapChain_Present(swap, 1, 0), "Present hr = ");
        frames++;

        if (key_esc || now - start >= RUN_MS) break;
    }

    wr("frames "); wnum((int64_t)frames);
    wr(" beeps "); wnum((int64_t)beeps);
    wr("\n");
    if (sv) IXAudio2SourceVoice_DestroyVoice(sv);
    if (mv) IXAudio2MasteringVoice_DestroyVoice(mv);
    if (xa) IXAudio2_Release(xa);
    wr("done\n");
    ExitProcess(0);
}

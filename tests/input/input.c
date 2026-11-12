/*
 * Win64 input test: a window that prints the keyboard and mouse messages it receives,
 * until WM_QUIT (posted by the Escape key) or a 10 second timeout.
 * tests/input/sendevents.c sends the events.
 */
#include <stdint.h>
#include <windows.h>

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

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void entry(void) {
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"xbemu_input";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"xbemu_input", L"xbemu input test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, NULL, NULL, NULL, NULL);
    if (!hwnd) ExitProcess(1);
    wr("ready\n");

    ULONGLONG start = GetTickCount64();
    MSG msg;
    int quit = 0;
    while (!quit && GetTickCount64() - start < 10000) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            switch (msg.message) {
            case WM_QUIT: quit = 1; break;
            case WM_KEYDOWN:
                wr("KEYDOWN "); wnum(msg.wParam); wr("\n");
                if (msg.wParam == 0x1B) PostQuitMessage(0);
                break;
            case WM_KEYUP: wr("KEYUP "); wnum(msg.wParam); wr("\n"); break;
            case WM_CHAR: wr("CHAR "); wnum(msg.wParam); wr("\n"); break;
            case WM_LBUTTONDOWN: wr("LDOWN "); wnum((short)LOWORD(msg.lParam)); wr(","); wnum((short)HIWORD(msg.lParam)); wr("\n"); break;
            case WM_LBUTTONUP: wr("LUP\n"); break;
            case WM_RBUTTONDOWN: wr("RDOWN\n"); break;
            case WM_MOUSEMOVE: wr("MOVE "); wnum((short)LOWORD(msg.lParam)); wr(","); wnum((short)HIWORD(msg.lParam)); wr("\n"); break;
            case WM_MOUSEWHEEL: wr("WHEEL "); wnum((short)HIWORD(msg.wParam)); wr("\n"); break;
            case WM_SETFOCUS: wr("FOCUS\n"); break;
            default: break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    wr("done\n");
    ExitProcess(0);
}

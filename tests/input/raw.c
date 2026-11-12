/*
 * Win64 raw input test: registers for raw mouse and keyboard input, then prints each raw
 * event it receives as WM_INPUT, until the key release (or a timeout).
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

/* Print one WM_INPUT record. Returns 1 when the final event (a key release) was printed. */
static int show(RAWINPUT *ri) {
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        RAWMOUSE *m = &ri->data.mouse;
        if (m->lLastX || m->lLastY) {
            wr("raw mouse "); wnum(m->lLastX); wr(" "); wnum(m->lLastY); wr("\n");
        }
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) wr("raw button 1 down\n");
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) wr("raw button 1 up\n");
        return 0;
    }
    if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        RAWKEYBOARD *k = &ri->data.keyboard;
        wr("raw key "); wnum(k->VKey);
        wr((k->Flags & RI_KEY_BREAK) ? " up\n" : " down\n");
        return (k->Flags & RI_KEY_BREAK) != 0;
    }
    return 0;
}

void entry(void) {
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"xbemu_raw";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"xbemu_raw", L"xbemu raw input", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, NULL, NULL, NULL, NULL);
    if (!hwnd) ExitProcess(1);

    RAWINPUTDEVICE devs[2] = {
        { 0x01, 0x02, 0, hwnd },   /* generic desktop: mouse */
        { 0x01, 0x06, 0, hwnd },   /* generic desktop: keyboard */
    };
    if (!RegisterRawInputDevices(devs, 2, sizeof devs[0])) { wr("register failed\n"); ExitProcess(2); }
    wr("ready\n");

    ULONGLONG start = GetTickCount64();
    MSG msg;
    while (GetTickCount64() - start < 8000) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_INPUT) {
                RAWINPUT ri;
                UINT size = sizeof ri;
                if (GetRawInputData((HRAWINPUT)msg.lParam, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == sizeof ri &&
                    show(&ri))
                    ExitProcess(0);
            }
            if (msg.message == WM_QUIT) ExitProcess(3);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    wr("timeout\n");
    ExitProcess(4);
}

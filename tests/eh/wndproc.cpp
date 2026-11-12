// C++ exception thrown in a window procedure, called from DispatchMessageW (host code),
// caught in the guest frame that called DispatchMessageW.
#include <stdexcept>
#include <windows.h>

extern "C" int atexit(void (*)(void)) { return 0; }

static void put(const char *s) {
    DWORD n = 0, w = 0;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &w, NULL);
}

struct Guard {
    int *count;
    explicit Guard(int *c) : count(c) {}
    ~Guard() { ++*count; }
};

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_USER) throw std::runtime_error("from window procedure");
    return DefWindowProcW(h, m, w, l);
}

extern "C" void entry(void) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = Proc;
    wc.lpszClassName = L"xbemu_eh";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"xbemu_eh", L"eh", 0, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    if (!hwnd) ExitProcess(1);

    int cleanups = 0;
    try {
        Guard g(&cleanups);
        MSG msg = {};
        msg.hwnd = hwnd;
        msg.message = WM_USER;
        DispatchMessageW(&msg);
        put("not reached\n");
    } catch (const std::exception &e) {
        put("caught: ");
        put(e.what());
        put(cleanups == 1 ? " cleanup ran\n" : " cleanup MISSING\n");
        ExitProcess(cleanups == 1 ? 0 : 3);
    }
    ExitProcess(4);
}

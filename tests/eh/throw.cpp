// Win64 C++ exception test: a throw crosses two frames, one of which has a destructor
// that must run during unwinding. The catch block reports what it caught.
#include <stdexcept>
#include <windows.h>

extern "C" int atexit(void (*)(void)) { return 0; } /* no CRT startup: nothing to run at exit */

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

__attribute__((noinline)) static void thrower(int x) {
    if (x) throw std::runtime_error("boom");
}

__attribute__((noinline)) static int middle(int *count, int x) {
    Guard g(count);
    thrower(x);
    return 1;
}

__attribute__((noinline)) static void throw_int(void) { throw 42; }

// The inner catch does not match an int, so the exception goes on to the outer catch(...).
__attribute__((noinline)) static int inner_mismatch(void) {
    try {
        throw_int();
    } catch (const std::exception &) {
        return 1; // must not run
    }
    return 2;
}

extern "C" void entry(void) {
    bool outer_ok = false;
    try {
        inner_mismatch();
    } catch (...) {
        outer_ok = true;
    }
    put(outer_ok ? "int passed through to catch(...)\n" : "int MISSED\n");

    int cleanups = 0;
    try {
        middle(&cleanups, 1);
        put("not reached\n");
    } catch (const std::exception &e) {
        put("caught: ");
        put(e.what());
        put(cleanups == 1 ? " cleanup ran\n" : " cleanup MISSING\n");
    }
    ExitProcess(cleanups == 1 && outer_ok ? 0 : 3);
}

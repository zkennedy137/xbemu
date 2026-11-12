/*
 * Win64 test program. Built with MinGW, no CRT, imports from kernel32.dll and
 * kernelx.dll. The string table below holds absolute pointers, so the loader
 * must apply base relocations to print it correctly.
 */
#include <stddef.h>
#include <stdint.h>

typedef uint32_t DWORD;
typedef void *HANDLE;
typedef int BOOL;

struct SystemOSVersion {
    uint8_t major, minor;
    uint16_t build, revision;
};

__declspec(dllimport) HANDLE GetStdHandle(DWORD);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) void ExitProcess(unsigned int) __attribute__((noreturn));

__declspec(dllimport) unsigned int GetConsoleType(void);
__declspec(dllimport) void GetSystemOSVersion(struct SystemOSVersion *);
__declspec(dllimport) void *XMemAlloc(size_t, uint64_t);
__declspec(dllimport) void XMemFree(void *, uint64_t);

static const char *const labels[] = {
    "hello from PE code\n",
    "console type: ",
    "guest OS: ",
    "alloc: ",
    "ok\n",
    "failed\n",
};

static void wr(const char *s) {
    DWORD n = 0;
    while (s[n]) n++;
    DWORD w = 0;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &w, NULL);
}

static void wnum(uint64_t v) {
    char b[24];
    int i = 23;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    wr(&b[i]);
}

void entry(void) {
    wr(labels[0]);

    wr(labels[1]);
    wnum(GetConsoleType());
    wr("\n");

    struct SystemOSVersion v;
    GetSystemOSVersion(&v);
    wr(labels[2]);
    wnum(v.major); wr("."); wnum(v.minor); wr("."); wnum(v.build); wr("."); wnum(v.revision);
    wr("\n");

    char *p = XMemAlloc(64, 0);
    wr(labels[3]);
    if (!p) {
        wr(labels[5]);
        ExitProcess(2);
    }
    p[0] = 'x';
    XMemFree(p, 0);
    wr(labels[4]);

    ExitProcess(7);
}

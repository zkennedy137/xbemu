/*
 * Win64 registry test. The first run stores a DWORD and a string under HKCU; a later run
 * (with the same XBEMU_REGISTRY file) finds them and checks their contents.
 */
#include <stdint.h>
#include <windows.h>

static void wr(const char *s) {
    DWORD n = 0, w = 0;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &w, NULL);
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void entry(void) {
    HKEY key = NULL;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\xbemu\\test", 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &key, NULL) != ERROR_SUCCESS)
        ExitProcess(1);

    DWORD value = 0;
    DWORD size = sizeof value, type = 0;
    LSTATUS st = RegQueryValueExA(key, "answer", NULL, &type, (LPBYTE)&value, &size);
    if (st == ERROR_SUCCESS && type == REG_DWORD && value == 42) {
        char name[64] = { 0 };
        size = sizeof name;
        if (RegQueryValueExA(key, "name", NULL, &type, (LPBYTE)name, &size) != ERROR_SUCCESS ||
            !streq(name, "xbemu")) {
            wr("string value wrong\n");
            ExitProcess(2);
        }
        wr("found stored values\n");
        RegCloseKey(key);
        ExitProcess(0);
    }

    DWORD answer = 42;
    if (RegSetValueExA(key, "answer", 0, REG_DWORD, (const BYTE *)&answer, sizeof answer) != ERROR_SUCCESS ||
        RegSetValueExA(key, "name", 0, REG_SZ, (const BYTE *)"xbemu", 6) != ERROR_SUCCESS) {
        wr("set failed\n");
        ExitProcess(3);
    }
    RegCloseKey(key);
    wr("stored values\n");
    ExitProcess(0);
}

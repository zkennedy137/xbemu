/*
 * Win64 threading test: four threads increment a counter under an SRW lock, and two
 * condition-variable handshakes run (critical section and SRW lock). Prints the results.
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

#define THREADS 4
#define ITERS 20000

static SRWLOCK srw = SRWLOCK_INIT;
static volatile LONG64 counter;

static DWORD WINAPI incrementer(LPVOID p) {
    (void)p;
    for (int i = 0; i < ITERS; i++) {
        AcquireSRWLockExclusive(&srw);
        counter++;
        ReleaseSRWLockExclusive(&srw);
    }
    return 0;
}

/* CS-based handshake: the waiter sleeps on a condition until the flag is set. */
static CRITICAL_SECTION cs;
static CONDITION_VARIABLE cv_cs;
static volatile int flag_cs;

static DWORD WINAPI setter_cs(LPVOID p) {
    (void)p;
    Sleep(50);
    EnterCriticalSection(&cs);
    flag_cs = 1;
    WakeConditionVariable(&cv_cs);
    LeaveCriticalSection(&cs);
    return 0;
}

/* SRW-based handshake. */
static SRWLOCK srw2 = SRWLOCK_INIT;
static CONDITION_VARIABLE cv_srw;
static volatile int flag_srw;

static DWORD WINAPI setter_srw(LPVOID p) {
    (void)p;
    Sleep(50);
    AcquireSRWLockExclusive(&srw2);
    flag_srw = 1;
    WakeAllConditionVariable(&cv_srw);
    ReleaseSRWLockExclusive(&srw2);
    return 0;
}

void entry(void) {
    HANDLE th[THREADS];
    for (int i = 0; i < THREADS; i++) th[i] = CreateThread(NULL, 0, incrementer, NULL, 0, NULL);
    WaitForSingleObject(th[0], INFINITE);
    for (int i = 1; i < THREADS; i++) WaitForSingleObject(th[i], INFINITE);
    wr("counter "); wnum(counter); wr(" expected "); wnum(THREADS * ITERS); wr("\n");

    InitializeCriticalSection(&cs);
    InitializeConditionVariable(&cv_cs);
    HANDLE t1 = CreateThread(NULL, 0, setter_cs, NULL, 0, NULL);
    EnterCriticalSection(&cs);
    while (!flag_cs) SleepConditionVariableCS(&cv_cs, &cs, INFINITE);
    LeaveCriticalSection(&cs);
    WaitForSingleObject(t1, INFINITE);
    wr(flag_cs ? "cs-handshake ok\n" : "cs-handshake FAILED\n");

    InitializeConditionVariable(&cv_srw);
    HANDLE t2 = CreateThread(NULL, 0, setter_srw, NULL, 0, NULL);
    AcquireSRWLockExclusive(&srw2);
    while (!flag_srw) SleepConditionVariableSRW(&cv_srw, &srw2, INFINITE, 0);
    ReleaseSRWLockExclusive(&srw2);
    WaitForSingleObject(t2, INFINITE);
    wr(flag_srw ? "srw-handshake ok\n" : "srw-handshake FAILED\n");

    int ok = counter == THREADS * ITERS && flag_cs && flag_srw;
    ExitProcess(ok ? 0 : 1);
}

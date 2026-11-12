/*
 * Win64 audio test: plays one second of a 440 Hz tone through XAudio2 in four buffers,
 * counts the start and end callbacks, and waits until all four have played.
 */
#define COBJMACROS
#include <stdint.h>
#include <windows.h>
#include <xaudio2.h>

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

#define RATE 48000
#define BUF_FRAMES 12000
#define BUFFERS 4

static int16_t pcm[BUFFERS][BUF_FRAMES * 2];
static volatile LONG started_count, ended_count;
static volatile LONG ended_ctx_sum;

/* IXAudio2VoiceCallback: this is the only callback object, so its vtable is static. */
static void STDMETHODCALLTYPE cb_pass_start(IXAudio2VoiceCallback *t, UINT32 bytes) { (void)t; (void)bytes; }
static void STDMETHODCALLTYPE cb_pass_end(IXAudio2VoiceCallback *t) { (void)t; }
static void STDMETHODCALLTYPE cb_stream_end(IXAudio2VoiceCallback *t) { (void)t; }
static void STDMETHODCALLTYPE cb_buffer_start(IXAudio2VoiceCallback *t, void *ctx) {
    (void)t; (void)ctx;
    InterlockedIncrement(&started_count);
}
static void STDMETHODCALLTYPE cb_buffer_end(IXAudio2VoiceCallback *t, void *ctx) {
    (void)t;
    InterlockedIncrement(&ended_count);
    InterlockedExchangeAdd(&ended_ctx_sum, (LONG)(intptr_t)ctx);
}
static void STDMETHODCALLTYPE cb_loop_end(IXAudio2VoiceCallback *t, void *ctx) { (void)t; (void)ctx; }
static void STDMETHODCALLTYPE cb_voice_error(IXAudio2VoiceCallback *t, void *ctx, HRESULT hr) {
    (void)t; (void)ctx; (void)hr;
}

static IXAudio2VoiceCallbackVtbl cb_vtbl = {
    cb_pass_start, cb_pass_end, cb_stream_end, cb_buffer_start, cb_buffer_end, cb_loop_end, cb_voice_error,
};
static IXAudio2VoiceCallback cb = { &cb_vtbl };

typedef HRESULT (WINAPI *PFN_XAudio2Create)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);

void entry(void) {
    HMODULE xa_dll = LoadLibraryA("xaudio2_9.dll");
    PFN_XAudio2Create create = xa_dll ? (PFN_XAudio2Create)GetProcAddress(xa_dll, "XAudio2Create") : NULL;
    if (!create) { wr("no XAudio2\n"); ExitProcess(1); }

    IXAudio2 *xa = NULL;
    if (FAILED(create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { wr("XAudio2Create failed\n"); ExitProcess(2); }

    IXAudio2MasteringVoice *mv = NULL;
    if (FAILED(IXAudio2_CreateMasteringVoice(xa, &mv, 2, RATE, 0, NULL, NULL, AudioCategory_GameEffects))) {
        wr("CreateMasteringVoice failed\n");
        ExitProcess(3);
    }

    WAVEFORMATEX wfx = { WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0 };
    IXAudio2SourceVoice *sv = NULL;
    if (FAILED(IXAudio2_CreateSourceVoice(xa, &sv, &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &cb, NULL, NULL))) {
        wr("CreateSourceVoice failed\n");
        ExitProcess(4);
    }

    /* Triangle wave at 440 Hz: phase step = 440 * 2^32 / 48000. Amplitude 8000. */
    uint32_t phase = 0;
    const uint32_t step = 39370534u;
    for (int b = 0; b < BUFFERS; b++) {
        for (int i = 0; i < BUF_FRAMES; i++) {
            uint32_t p = phase >> 16;
            int32_t tri = p < 32768 ? (int32_t)p : (int32_t)(65535 - p);   /* 0..32767 */
            int16_t v = (int16_t)(((tri * 2 - 32768) * 8000) / 32768);
            pcm[b][i * 2] = v;
            pcm[b][i * 2 + 1] = v;
            phase += step;
        }
    }

    for (int b = 0; b < BUFFERS; b++) {
        XAUDIO2_BUFFER buf = { 0 };
        buf.AudioBytes = BUF_FRAMES * 4;
        buf.pAudioData = (const BYTE *)pcm[b];
        buf.pContext = (void *)(intptr_t)(b + 1);
        if (b == BUFFERS - 1) buf.Flags = XAUDIO2_END_OF_STREAM;
        if (FAILED(IXAudio2SourceVoice_SubmitSourceBuffer(sv, &buf, NULL))) {
            wr("SubmitSourceBuffer failed\n");
            ExitProcess(5);
        }
    }
    IXAudio2SourceVoice_Start(sv, 0, XAUDIO2_COMMIT_NOW);

    ULONGLONG start = GetTickCount64();
    while (ended_count < BUFFERS && GetTickCount64() - start < 8000)
        Sleep(10);

    wr("started "); wnum(started_count); wr(" ended "); wnum(ended_count);
    wr(" ctx-sum "); wnum(ended_ctx_sum); wr("\n");
    IXAudio2SourceVoice_DestroyVoice(sv);
    IXAudio2MasteringVoice_DestroyVoice(mv);
    IXAudio2_Release(xa);
    wr("done\n");
    ExitProcess(ended_count == BUFFERS ? 0 : 6);
}

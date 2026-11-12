/*
 * Win64 XAudio2 test: two engines in one process, each with its own mastering voice and
 * source voice. Both must play their buffer to the end.
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

#define RATE 48000
#define FRAMES 24000
static int16_t pcm[2][FRAMES * 2];
static volatile LONG ended[2];

static void STDMETHODCALLTYPE cb_pass_start(IXAudio2VoiceCallback *t, UINT32 b) { (void)t; (void)b; }
static void STDMETHODCALLTYPE cb_pass_end(IXAudio2VoiceCallback *t) { (void)t; }
static void STDMETHODCALLTYPE cb_stream_end(IXAudio2VoiceCallback *t) { (void)t; }
static void STDMETHODCALLTYPE cb_buffer_start(IXAudio2VoiceCallback *t, void *ctx) { (void)t; (void)ctx; }
static void STDMETHODCALLTYPE cb_buffer_end(IXAudio2VoiceCallback *t, void *ctx) {
    (void)t;
    InterlockedIncrement(&ended[(intptr_t)ctx]);
}
static void STDMETHODCALLTYPE cb_loop_end(IXAudio2VoiceCallback *t, void *ctx) { (void)t; (void)ctx; }
static void STDMETHODCALLTYPE cb_voice_error(IXAudio2VoiceCallback *t, void *ctx, HRESULT hr) { (void)t; (void)ctx; (void)hr; }
static IXAudio2VoiceCallbackVtbl cb_vtbl = { cb_pass_start, cb_pass_end, cb_stream_end, cb_buffer_start, cb_buffer_end, cb_loop_end, cb_voice_error };
static IXAudio2VoiceCallback cb = { &cb_vtbl };

typedef HRESULT (WINAPI *PFN_XAudio2Create)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);

void entry(void) {
    HMODULE dll = LoadLibraryA("xaudio2_9.dll");
    PFN_XAudio2Create create = dll ? (PFN_XAudio2Create)GetProcAddress(dll, "XAudio2Create") : NULL;
    if (!create) { wr("no XAudio2\n"); ExitProcess(1); }

    IXAudio2 *xa[2] = { NULL, NULL };
    IXAudio2MasteringVoice *mv[2] = { NULL, NULL };
    IXAudio2SourceVoice *sv[2] = { NULL, NULL };
    WAVEFORMATEX wfx = { WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0 };
    /* Engine 0 plays 440 Hz, engine 1 plays 660 Hz: step = freq * 2^32 / 48000. */
    const uint32_t steps[2] = { 39370534u, 59055802u };

    for (int e = 0; e < 2; e++) {
        if (FAILED(create(&xa[e], 0, XAUDIO2_DEFAULT_PROCESSOR))) { wr("XAudio2Create failed\n"); ExitProcess(2); }
        IXAudio2_CreateMasteringVoice(xa[e], &mv[e], 2, RATE, 0, NULL, NULL, AudioCategory_GameEffects);
        if (FAILED(IXAudio2_CreateSourceVoice(xa[e], &sv[e], &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &cb, NULL, NULL))) {
            wr("CreateSourceVoice failed\n");
            ExitProcess(3);
        }
        uint32_t phase = 0;
        for (int i = 0; i < FRAMES; i++) {
            uint32_t p = phase >> 16;
            int32_t tri = p < 32768 ? (int32_t)p : (int32_t)(65535 - p);
            int16_t v = (int16_t)(((tri * 2 - 32768) * 8000) / 32768);
            pcm[e][i * 2] = v;
            pcm[e][i * 2 + 1] = v;
            phase += steps[e];
        }
        XAUDIO2_BUFFER buf = { 0 };
        buf.AudioBytes = FRAMES * 4;
        buf.pAudioData = (const BYTE *)pcm[e];
        buf.pContext = (void *)(intptr_t)e;
        buf.Flags = XAUDIO2_END_OF_STREAM;
        IXAudio2SourceVoice_SubmitSourceBuffer(sv[e], &buf, NULL);
        IXAudio2SourceVoice_Start(sv[e], 0, XAUDIO2_COMMIT_NOW);
    }

    ULONGLONG start = GetTickCount64();
    while ((ended[0] < 1 || ended[1] < 1) && GetTickCount64() - start < 8000) Sleep(10);
    int ok = ended[0] == 1 && ended[1] == 1;
    wr(ok ? "both engines played\n" : "an engine did not finish\n");

    for (int e = 0; e < 2; e++) {
        IXAudio2SourceVoice_DestroyVoice(sv[e]);
        IXAudio2MasteringVoice_DestroyVoice(mv[e]);
        IXAudio2_Release(xa[e]);
    }
    ExitProcess(ok ? 0 : 4);
}

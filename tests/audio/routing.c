/*
 * Win64 XAudio2 routing test. Source A (440 Hz) goes to a submix with channel volumes
 * (1, 0), so it is heard only on the left. Source B (880 Hz) goes straight to the output.
 * Expected: the right channel holds only 880 Hz; the left channel holds both tones.
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
#define FRAMES 48000

static int16_t pcm_a[FRAMES * 2], pcm_b[FRAMES * 2];
static volatile LONG ended;

static void STDMETHODCALLTYPE cb_pass_start(IXAudio2VoiceCallback *t, UINT32 b) { (void)t; (void)b; }
static void STDMETHODCALLTYPE cb_pass_end(IXAudio2VoiceCallback *t) { (void)t; }
static void STDMETHODCALLTYPE cb_stream_end(IXAudio2VoiceCallback *t) { (void)t; }
static void STDMETHODCALLTYPE cb_buffer_start(IXAudio2VoiceCallback *t, void *ctx) { (void)t; (void)ctx; }
static void STDMETHODCALLTYPE cb_buffer_end(IXAudio2VoiceCallback *t, void *ctx) { (void)t; (void)ctx; InterlockedIncrement(&ended); }
static void STDMETHODCALLTYPE cb_loop_end(IXAudio2VoiceCallback *t, void *ctx) { (void)t; (void)ctx; }
static void STDMETHODCALLTYPE cb_voice_error(IXAudio2VoiceCallback *t, void *ctx, HRESULT hr) { (void)t; (void)ctx; (void)hr; }
static IXAudio2VoiceCallbackVtbl cb_vtbl = { cb_pass_start, cb_pass_end, cb_stream_end, cb_buffer_start, cb_buffer_end, cb_loop_end, cb_voice_error };
static IXAudio2VoiceCallback cb = { &cb_vtbl };

/* Triangle wave with phase step = freq * 2^32 / 48000, amplitude 8000, both channels. */
static void fill_tone(int16_t *out, uint32_t step) {
    uint32_t phase = 0;
    for (int i = 0; i < FRAMES; i++) {
        uint32_t p = phase >> 16;
        int32_t tri = p < 32768 ? (int32_t)p : (int32_t)(65535 - p);
        int16_t v = (int16_t)(((tri * 2 - 32768) * 8000) / 32768);
        out[i * 2] = v;
        out[i * 2 + 1] = v;
        phase += step;
    }
}

typedef HRESULT (WINAPI *PFN_XAudio2Create)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);

void entry(void) {
    HMODULE dll = LoadLibraryA("xaudio2_9.dll");
    PFN_XAudio2Create create = dll ? (PFN_XAudio2Create)GetProcAddress(dll, "XAudio2Create") : NULL;
    IXAudio2 *xa = NULL;
    if (!create || FAILED(create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { wr("no XAudio2\n"); ExitProcess(1); }

    IXAudio2MasteringVoice *mv = NULL;
    IXAudio2_CreateMasteringVoice(xa, &mv, 2, RATE, 0, NULL, NULL, AudioCategory_GameEffects);

    IXAudio2SubmixVoice *sub = NULL;
    if (FAILED(IXAudio2_CreateSubmixVoice(xa, &sub, 2, RATE, 0, 0, NULL, NULL))) { wr("submix failed\n"); ExitProcess(2); }

    WAVEFORMATEX wfx = { WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0 };
    IXAudio2SourceVoice *a = NULL, *b = NULL;
    IXAudio2_CreateSourceVoice(xa, &a, &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &cb, NULL, NULL);
    IXAudio2_CreateSourceVoice(xa, &b, &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &cb, NULL, NULL);

    /* The submix plays at half volume, so its contribution to the left channel is visible. */
    IXAudio2SubmixVoice_SetVolume(sub, 0.5f, XAUDIO2_COMMIT_NOW);

    /* A: route to the submix, and use only the left channel. */
    XAUDIO2_SEND_DESCRIPTOR send = { 0, (IXAudio2Voice *)sub };
    XAUDIO2_VOICE_SENDS sa = { 1, &send };
    IXAudio2SourceVoice_SetOutputVoices(a, &sa);
    const float left_only[2] = { 1.0f, 0.0f };
    IXAudio2SourceVoice_SetChannelVolumes(a, 2, left_only, XAUDIO2_COMMIT_NOW);

    fill_tone(pcm_a, 39370534u);    /* 440 Hz */
    fill_tone(pcm_b, 78741068u);    /* 880 Hz */

    XAUDIO2_BUFFER ba = { 0 }, bb = { 0 };
    ba.AudioBytes = FRAMES * 4;  ba.pAudioData = (const BYTE *)pcm_a;  ba.Flags = XAUDIO2_END_OF_STREAM;
    bb.AudioBytes = FRAMES * 4;  bb.pAudioData = (const BYTE *)pcm_b;  bb.Flags = XAUDIO2_END_OF_STREAM;
    IXAudio2SourceVoice_SubmitSourceBuffer(a, &ba, NULL);
    IXAudio2SourceVoice_SubmitSourceBuffer(b, &bb, NULL);
    IXAudio2SourceVoice_Start(a, 0, XAUDIO2_COMMIT_NOW);
    IXAudio2SourceVoice_Start(b, 0, XAUDIO2_COMMIT_NOW);

    ULONGLONG start = GetTickCount64();
    while (ended < 2 && GetTickCount64() - start < 8000) Sleep(10);
    wr(ended == 2 ? "routing played\n" : "routing did not finish\n");

    IXAudio2SourceVoice_DestroyVoice(a);
    IXAudio2SourceVoice_DestroyVoice(b);
    IXAudio2SubmixVoice_DestroyVoice(sub);
    IXAudio2MasteringVoice_DestroyVoice(mv);
    IXAudio2_Release(xa);
    ExitProcess(ended == 2 ? 0 : 3);
}

/*
 * XAudio2 (xaudio2_9.dll, xaudio2_8.dll): engine, mastering voice, source voices.
 *
 * Guest code sees COM objects whose vtables hold guest stubs (w64_make_stub), so calls
 * through them use the Win64 convention. The vtable slot numbers follow xaudio2.h.
 *
 * Each engine has a host thread that mixes its started source voices into 10 ms periods at
 * 48 kHz stereo and writes them to its audio output (audio_out.c). Buffers
 * complete on that thread, which calls the guest's IXAudio2VoiceCallback.
 *
 * Source formats: PCM 8/16/32-bit, IEEE float 32-bit, and WAVE_FORMAT_EXTENSIBLE with
 * either subformat. Sample rates are converted by nearest-neighbour resampling.
 * Submix voices and per-channel volumes work. Effects and filters are accepted, not applied.
 */
#define _DEFAULT_SOURCE
#include "hostlib.h"
#include "audio_out.h"
#include "../teb.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HR_OK          0u
#define HR_INVALIDARG  0x80070057u
#define HR_FAIL        0x80004005u
#define HR_NOIFACE     0x80004002u
#define HR_NOTIMPL     0x80004001u
#define XA_INVALID_CALL 0x88960001u

#define OUT_RATE 48000u
#define OUT_CH   2u
#define PERIOD_FRAMES 480u    /* 10 ms */
#define MAX_QUEUE   64
#define MAX_SOURCES 64
#define MAX_BUSES   16

#define XA_END_OF_STREAM 0x40u

/* Vtable slot counts: IXAudio2Voice (19), IXAudio2SourceVoice (28), IXAudio2 (13). */
#define BASE_SLOTS 19
#define SRC_SLOTS  28
#define ENG_SLOTS  13
/* IXAudio2VoiceCallback slots used here. */
#define CB_BUFFER_START 3
#define CB_BUFFER_END   4

typedef struct QEntry {
    const uint8_t *data;
    uint32_t bytes;
    void *ctx;
    double cursor;   /* position in source frames */
    int begun;
} QEntry;

struct Engine;

typedef struct Source {
    void *vtbl;      /* first: the COM object pointer is the Source pointer */
    struct Engine *eng;     /* the engine this voice belongs to */
    int kind;        /* 1 = source voice, 2 = mastering voice, 3 = submix voice */
    struct Source *target;  /* where a source's output goes: a submix, or NULL for master */
    float ch_vol[2];        /* per-channel volume: left, right */
    float *bus_acc;         /* submix: its mix buffer, PERIOD_FRAMES stereo floats */
    uint32_t channels, rate, bits;
    int is_float;
    uint32_t frame_bytes;
    float volume;
    float freq;      /* frequency ratio set by the application */
    void *callback;
    int started;
    QEntry q[MAX_QUEUE];
    int qhead, qcount;
    uint64_t frames_played;
} Source;

typedef struct Engine {
    void *vtbl;
    int refs;
    AudioOut *out;
    pthread_t thr;
    int running;     /* StartEngine/StopEngine */
    int quit;
    float master_gain;   /* mastering voice volume */
} Engine;

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static Source *g_sources[MAX_SOURCES];
static Source *g_buses[MAX_BUSES];

static int vtables_ready;
static void *base_vtbl[BASE_SLOTS];
static void *src_vtbl[SRC_SLOTS];
static void *eng_vtbl[ENG_SLOTS];

/* ---- helpers ---- */

static void set_slot(void **v, int i, const char *dll, const char *name, W64Fn fn) {
    v[i] = w64_make_stub(dll, name, fn);
}

static void add_source(Source *s) {
    for (int i = 0; i < MAX_SOURCES; i++)
        if (!g_sources[i]) { g_sources[i] = s; return; }
}

static void remove_source(Source *s) {
    for (int i = 0; i < MAX_SOURCES; i++)
        if (g_sources[i] == s) g_sources[i] = NULL;
}

/* Sample `ch` of a frame, as float in [-1, 1]. */
static float sample_at(const Source *s, const uint8_t *frame, unsigned ch) {
    if (s->is_float) {
        float f;
        memcpy(&f, frame + ch * 4, 4);
        return f;
    }
    switch (s->bits) {
    case 8:  return ((int)frame[ch] - 128) / 128.0f;
    case 16: { int16_t v; memcpy(&v, frame + ch * 2, 2); return v / 32768.0f; }
    case 32: { int32_t v; memcpy(&v, frame + ch * 4, 4); return v / 2147483648.0f; }
    default: return 0.0f;
    }
}

/* Stereo view of a frame: mono is duplicated, more than two channels use the first two. */
static void frame_lr(const Source *s, const uint8_t *frame, float *l, float *r) {
    *l = sample_at(s, frame, 0);
    *r = s->channels >= 2 ? sample_at(s, frame, 1) : *l;
}

/* ---- mixing ---- */

typedef struct Callback {
    void *obj;
    void *ctx;
    int slot;
} Callback;

#define MAX_CALLBACKS (MAX_SOURCES * MAX_QUEUE * 2)

/* Mix one source into acc (PERIOD_FRAMES stereo floats). Caller holds g_m. */
static void mix_source(Source *s, float *acc, Callback *cbs, int *ncb) {
    double step = (double)s->rate / OUT_RATE * s->freq;
    unsigned produced = 0;
    while (produced < PERIOD_FRAMES && s->qcount > 0) {
        QEntry *q = &s->q[s->qhead];
        uint64_t total = q->bytes / s->frame_bytes;
        if (!q->begun) {
            q->begun = 1;
            if (s->callback && *ncb < MAX_CALLBACKS)
                cbs[(*ncb)++] = (Callback){ s->callback, q->ctx, CB_BUFFER_START };
        }
        while (produced < PERIOD_FRAMES && q->cursor < (double)total) {
            size_t fi = (size_t)q->cursor;
            float l, r;
            frame_lr(s, q->data + fi * s->frame_bytes, &l, &r);
            acc[produced * 2] += l * s->volume * s->ch_vol[0];
            acc[produced * 2 + 1] += r * s->volume * s->ch_vol[1];
            produced++;
            q->cursor += step;
            s->frames_played++;
        }
        if (q->cursor < (double)total) break;   /* out of output frames for this period */
        if (s->callback && *ncb < MAX_CALLBACKS)
            cbs[(*ncb)++] = (Callback){ s->callback, q->ctx, CB_BUFFER_END };
        s->qhead = (s->qhead + 1) % MAX_QUEUE;
        s->qcount--;
    }
}

/* Call a guest IXAudio2VoiceCallback method: OnBufferStart (3) or OnBufferEnd (4). */
static void call_callback(const Callback *cb) {
    void **vtbl = *(void ***)cb->obj;
    void *fn = vtbl[cb->slot];
    if (fn) xbemu_call_w64(fn, (uint64_t)(uintptr_t)cb->obj, (uint64_t)(uintptr_t)cb->ctx, 0, 0);
}

static void *mix_thread(void *arg) {
    Engine *e = arg;
    teb_init_current_thread();
    /* Per-thread scratch: each engine has its own mixer thread. */
    int16_t *outbuf = malloc(PERIOD_FRAMES * OUT_CH * sizeof *outbuf);
    float *acc = malloc(PERIOD_FRAMES * OUT_CH * sizeof *acc);
    Callback *cbs = malloc(MAX_CALLBACKS * sizeof *cbs);
    if (!outbuf || !acc || !cbs) return NULL;

    for (;;) {
        pthread_mutex_lock(&g_m);
        if (e->quit) { pthread_mutex_unlock(&g_m); break; }
        if (!e->running) {
            pthread_mutex_unlock(&g_m);
            struct timespec ts = { 0, 5 * 1000000L };
            nanosleep(&ts, NULL);
            continue;
        }
        memset(acc, 0, PERIOD_FRAMES * OUT_CH * sizeof *acc);
        int ncb = 0;
        /* Submixes first: each mixes its own sources, then adds to the output. */
        for (int b = 0; b < MAX_BUSES; b++) {
            Source *bus = g_buses[b];
            if (!bus || bus->eng != e) continue;
            memset(bus->bus_acc, 0, PERIOD_FRAMES * OUT_CH * sizeof(float));
            for (int i = 0; i < MAX_SOURCES; i++) {
                Source *s = g_sources[i];
                if (s && s->eng == e && s->target == bus && s->started && s->qcount > 0)
                    mix_source(s, bus->bus_acc, cbs, &ncb);
            }
            for (size_t i = 0; i < PERIOD_FRAMES * OUT_CH; i++)
                acc[i] += bus->bus_acc[i] * bus->volume;
        }
        /* Sources that go straight to the output. */
        for (int i = 0; i < MAX_SOURCES; i++) {
            Source *s = g_sources[i];
            if (s && s->eng == e && s->kind == 1 && s->target == NULL && s->started && s->qcount > 0)
                mix_source(s, acc, cbs, &ncb);
        }
        float master = e->master_gain;
        pthread_mutex_unlock(&g_m);

        for (size_t i = 0; i < PERIOD_FRAMES * OUT_CH; i++) {
            float v = acc[i] * master;
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            outbuf[i] = (int16_t)(v * 32767.0f);
        }
        audio_out_write(e->out, outbuf, PERIOD_FRAMES);

        /* Callbacks run outside the lock, because guest code may call back into XAudio2. */
        for (int i = 0; i < ncb; i++) call_callback(&cbs[i]);
    }
    free(outbuf);
    free(acc);
    free(cbs);
    return NULL;
}

/* ---- IUnknown-style and engine methods ---- */

static uint64_t e_QueryInterface(W64Ctx *c) {
    void **ppv = (void **)(uintptr_t)w64_arg(c, 2);
    if (ppv) *ppv = NULL;
    return HR_NOIFACE;
}

static uint64_t e_AddRef(W64Ctx *c) {
    Engine *e = (Engine *)(uintptr_t)w64_arg(c, 0);
    return (uint32_t)++e->refs;
}

static void engine_destroy(Engine *e) {
    pthread_mutex_lock(&g_m);
    e->quit = 1;
    pthread_mutex_unlock(&g_m);
    pthread_join(e->thr, NULL);
    audio_out_close(e->out);
    free(e);
}

static uint64_t e_Release(W64Ctx *c) {
    Engine *e = (Engine *)(uintptr_t)w64_arg(c, 0);
    int refs = --e->refs;
    if (refs == 0) engine_destroy(e);
    return (uint32_t)refs;
}

static uint64_t e_GetDeviceCount(W64Ctx *c) {
    uint32_t *count = (uint32_t *)(uintptr_t)w64_arg(c, 1);
    if (count) *count = 1;
    return HR_OK;
}

static uint64_t e_NotImplemented(W64Ctx *c) {
    (void)c;
    return HR_NOTIMPL;
}

static uint64_t e_StartEngine(W64Ctx *c) {
    Engine *e = (Engine *)(uintptr_t)w64_arg(c, 0);
    pthread_mutex_lock(&g_m);
    e->running = 1;
    pthread_mutex_unlock(&g_m);
    return HR_OK;
}

static uint64_t e_StopEngine(W64Ctx *c) {
    Engine *e = (Engine *)(uintptr_t)w64_arg(c, 0);
    pthread_mutex_lock(&g_m);
    e->running = 0;
    pthread_mutex_unlock(&g_m);
    return HR_OK;
}

static uint64_t ok(W64Ctx *c) {
    (void)c;
    return HR_OK;
}

/* Parse a WAVEFORMATEX (or WAVEFORMATEXTENSIBLE) into a voice's format fields. */
static int parse_format(Source *s, const uint8_t *f) {
    uint16_t tag, ch, bits;
    uint32_t rate;
    memcpy(&tag, f, 2);
    memcpy(&ch, f + 2, 2);
    memcpy(&rate, f + 4, 4);
    memcpy(&bits, f + 14, 2);
    s->is_float = 0;
    if (tag == 3) s->is_float = 1;
    else if (tag == 0xFFFE) {
        uint16_t sub;
        memcpy(&sub, f + 24, 2);  /* SubFormat GUID, first 2 bytes */
        if (sub == 3) s->is_float = 1;
        else if (sub != 1) return 0;
    } else if (tag != 1) {
        return 0;
    }
    if (ch == 0) return 0;
    if (s->is_float ? bits != 32 : !(bits == 8 || bits == 16 || bits == 32)) return 0;
    s->channels = ch;
    s->rate = rate;
    s->bits = bits;
    s->frame_bytes = (uint32_t)ch * (bits / 8);
    return rate != 0;
}

/* HRESULT CreateSourceVoice(IXAudio2SourceVoice **, const WAVEFORMATEX *, UINT32 flags,
 *                           float maxFreqRatio, IXAudio2VoiceCallback *, const XAUDIO2_VOICE_SENDS *,
 *                           const XAUDIO2_EFFECT_CHAIN *) */
static uint64_t e_CreateSourceVoice(W64Ctx *c) {
    void **out = (void **)(uintptr_t)w64_arg(c, 1);
    const uint8_t *fmt = (const uint8_t *)(uintptr_t)w64_arg(c, 2);
    if (!out || !fmt) return HR_INVALIDARG;
    Source *s = calloc(1, sizeof *s);
    if (!s) return HR_FAIL;
    s->eng = (Engine *)(uintptr_t)w64_arg(c, 0);
    s->vtbl = src_vtbl;
    s->kind = 1;
    s->volume = 1.0f;
    s->freq = 1.0f;
    s->ch_vol[0] = s->ch_vol[1] = 1.0f;
    if (!parse_format(s, fmt)) { free(s); return HR_INVALIDARG; }
    s->callback = (void *)(uintptr_t)w64_arg(c, 5);

    pthread_mutex_lock(&g_m);
    add_source(s);
    pthread_mutex_unlock(&g_m);
    *out = s;
    return HR_OK;
}

/* HRESULT CreateSubmixVoice(IXAudio2SubmixVoice **, UINT32 channels, UINT32 rate, UINT32 flags,
 *                           UINT32 stage, const XAUDIO2_VOICE_SENDS *, const XAUDIO2_EFFECT_CHAIN *)
 * A submix mixes the sources routed to it and passes the result on (to master, or to another
 * submix). Effects are not applied. */
static uint64_t e_CreateSubmixVoice(W64Ctx *c) {
    void **out = (void **)(uintptr_t)w64_arg(c, 1);
    if (!out) return HR_INVALIDARG;
    Source *bus = calloc(1, sizeof *bus);
    if (!bus) return HR_FAIL;
    bus->eng = (Engine *)(uintptr_t)w64_arg(c, 0);
    bus->bus_acc = calloc(PERIOD_FRAMES * OUT_CH, sizeof(float));
    if (!bus->bus_acc) { free(bus); return HR_FAIL; }
    bus->vtbl = base_vtbl;
    bus->kind = 3;
    bus->channels = OUT_CH;
    bus->rate = OUT_RATE;
    bus->volume = 1.0f;
    bus->ch_vol[0] = bus->ch_vol[1] = 1.0f;
    pthread_mutex_lock(&g_m);
    int slot = -1;
    for (int b = 0; b < MAX_BUSES; b++)
        if (!g_buses[b]) { g_buses[b] = bus; slot = b; break; }
    pthread_mutex_unlock(&g_m);
    if (slot < 0) { free(bus->bus_acc); free(bus); return HR_FAIL; }
    *out = bus;
    return HR_OK;
}

/* HRESULT CreateMasteringVoice(IXAudio2MasteringVoice **, UINT32 channels, UINT32 rate, ...)
 * The output format is fixed (48 kHz stereo), so the request is accepted as is. */
static uint64_t e_CreateMasteringVoice(W64Ctx *c) {
    void **out = (void **)(uintptr_t)w64_arg(c, 1);
    if (!out) return HR_INVALIDARG;
    Source *s = calloc(1, sizeof *s);
    if (!s) return HR_FAIL;
    s->eng = (Engine *)(uintptr_t)w64_arg(c, 0);
    s->vtbl = base_vtbl;
    s->kind = 2;
    s->channels = OUT_CH;
    s->rate = OUT_RATE;
    *out = s;
    return HR_OK;
}

/* ---- voice methods (base IXAudio2Voice) ---- */

/* void GetVoiceDetails(XAUDIO2_VOICE_DETAILS *): {CreationFlags, ActiveFlags, InputChannels, InputSampleRate} */
static uint64_t v_GetVoiceDetails(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    uint32_t *d = (uint32_t *)(uintptr_t)w64_arg(c, 1);
    if (!d) return 0;
    d[0] = 0;
    d[1] = 0;
    d[2] = s->channels;
    d[3] = s->rate;
    return 0;
}

static uint64_t v_SetVolume(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    float v = w64_arg_f32(c, 1);
    pthread_mutex_lock(&g_m);
    if (s->kind == 2) s->eng->master_gain = v;   /* the mastering voice's volume is the output gain */
    else s->volume = v;
    pthread_mutex_unlock(&g_m);
    return 0;
}

/* HRESULT SetChannelVolumes(UINT32 channels, const float *volumes, UINT32 opSet): the first
 * two values apply to left and right. A mono source uses the first value for both. */
static uint64_t v_SetChannelVolumes(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    uint32_t n = (uint32_t)w64_arg(c, 1);
    const float *v = (const float *)(uintptr_t)w64_arg(c, 2);
    if (!v || n == 0) return HR_INVALIDARG;
    pthread_mutex_lock(&g_m);
    s->ch_vol[0] = v[0];
    s->ch_vol[1] = n > 1 ? v[1] : v[0];
    pthread_mutex_unlock(&g_m);
    return 0;
}

/* void GetChannelVolumes(UINT32 channels, float *volumes) */
static uint64_t v_GetChannelVolumes(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    uint32_t n = (uint32_t)w64_arg(c, 1);
    float *v = (float *)(uintptr_t)w64_arg(c, 2);
    if (!v || n == 0) return 0;
    v[0] = s->ch_vol[0];
    if (n > 1) v[1] = s->ch_vol[1];
    return 0;
}

/* HRESULT SetOutputVoices(const XAUDIO2_VOICE_SENDS *)
 * XAUDIO2_VOICE_SENDS is {UINT32 SendCount; XAUDIO2_SEND_DESCRIPTOR *pSends} packed to 4 bytes.
 * XAUDIO2_SEND_DESCRIPTOR is {UINT32 Flags; IXAudio2Voice *pOutputVoice}, 12 bytes packed.
 * The first submix among the sends is the target. No submix means the output. */
static uint64_t v_SetOutputVoices(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    const uint8_t *sends = (const uint8_t *)(uintptr_t)w64_arg(c, 1);
    Source *target = NULL;
    if (sends) {
        uint32_t count;
        const uint8_t *desc;
        memcpy(&count, sends, 4);
        memcpy(&desc, sends + 4, 8);
        for (uint32_t i = 0; desc && i < count && !target; i++) {
            Source *t;
            memcpy(&t, desc + i * 12 + 4, 8);   /* pOutputVoice */
            if (t && t->kind == 3) target = t;
        }
    }
    pthread_mutex_lock(&g_m);
    s->target = target;
    pthread_mutex_unlock(&g_m);
    return 0;
}

static uint64_t v_GetVolume(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    float *out = (float *)(uintptr_t)w64_arg(c, 1);
    if (out) *out = s->volume;
    return 0;
}

static uint64_t v_GetEffectState(W64Ctx *c) {
    int32_t *enabled = (int32_t *)(uintptr_t)w64_arg(c, 2);
    if (enabled) *enabled = 0;
    return 0;
}

static uint64_t v_DestroyVoice(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    pthread_mutex_lock(&g_m);
    if (s->kind == 1) remove_source(s);
    if (s->kind == 3) {
        for (int b = 0; b < MAX_BUSES; b++) if (g_buses[b] == s) g_buses[b] = NULL;
        for (int i = 0; i < MAX_SOURCES; i++) if (g_sources[i] && g_sources[i]->target == s) g_sources[i]->target = NULL;
    }
    pthread_mutex_unlock(&g_m);
    free(s->bus_acc);
    free(s);
    return 0;
}

/* ---- source voice methods ---- */

/* void Start(UINT32 flags, UINT32 opSet) */
static uint64_t s_Start(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    pthread_mutex_lock(&g_m);
    s->started = 1;
    pthread_mutex_unlock(&g_m);
    return 0;
}

static uint64_t s_Stop(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    pthread_mutex_lock(&g_m);
    s->started = 0;
    pthread_mutex_unlock(&g_m);
    return 0;
}

/* HRESULT SubmitSourceBuffer(const XAUDIO2_BUFFER *, const XAUDIO2_BUFFER_WMA *)
 * XAUDIO2_BUFFER is packed to 4-byte alignment (44 bytes):
 * Flags 0, AudioBytes 4, pAudioData 8, pContext 36. */
static uint64_t s_SubmitSourceBuffer(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    const uint8_t *b = (const uint8_t *)(uintptr_t)w64_arg(c, 1);
    if (!b) return HR_INVALIDARG;
    uint32_t bytes;
    const uint8_t *data;
    void *ctx;
    memcpy(&bytes, b + 4, 4);
    memcpy(&data, b + 8, 8);
    memcpy(&ctx, b + 36, 8);
    if (!data || bytes == 0 || bytes % s->frame_bytes != 0) return HR_INVALIDARG;

    pthread_mutex_lock(&g_m);
    if (s->qcount == MAX_QUEUE) {
        pthread_mutex_unlock(&g_m);
        return XA_INVALID_CALL;
    }
    QEntry *q = &s->q[(s->qhead + s->qcount) % MAX_QUEUE];
    q->data = data;
    q->bytes = bytes;
    q->ctx = ctx;
    q->cursor = 0.0;
    q->begun = 0;
    s->qcount++;
    pthread_mutex_unlock(&g_m);
    return HR_OK;
}

/* HRESULT FlushSourceBuffers(void): queued buffers are dropped without callbacks. */
static uint64_t s_FlushSourceBuffers(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    pthread_mutex_lock(&g_m);
    s->qhead = 0;
    s->qcount = 0;
    pthread_mutex_unlock(&g_m);
    return HR_OK;
}

/* void GetState(XAUDIO2_VOICE_STATE *, UINT32 flags): packed, 20 bytes:
 * {pCurrentBufferContext 0, BuffersQueued 8, SamplesPlayed 12} */
static uint64_t s_GetState(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    uint8_t *st = (uint8_t *)(uintptr_t)w64_arg(c, 1);
    if (!st) return 0;
    pthread_mutex_lock(&g_m);
    void *cur = s->qcount ? s->q[s->qhead].ctx : NULL;
    uint32_t queued = (uint32_t)s->qcount;
    uint64_t played = s->frames_played;
    pthread_mutex_unlock(&g_m);
    memset(st, 0, 20);
    memcpy(st + 0, &cur, 8);
    memcpy(st + 8, &queued, 4);
    memcpy(st + 12, &played, 8);
    return 0;
}

static uint64_t s_SetFrequencyRatio(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    float r = w64_arg_f32(c, 1);
    if (r > 0.0f) s->freq = r;
    return 0;
}

static uint64_t s_GetFrequencyRatio(W64Ctx *c) {
    Source *s = (Source *)(uintptr_t)w64_arg(c, 0);
    float *out = (float *)(uintptr_t)w64_arg(c, 1);
    if (out) *out = s->freq;
    return 0;
}

/* ---- vtables ---- */

static void init_vtables(void) {
    if (vtables_ready) return;
    vtables_ready = 1;
    static const char *dll = "xaudio2_9.dll";
    /* Unlisted slots log "unimplemented" when called. */
    for (int i = 0; i < BASE_SLOTS; i++) set_slot(base_vtbl, i, dll, "IXAudio2Voice", NULL);
    for (int i = 0; i < SRC_SLOTS; i++) set_slot(src_vtbl, i, dll, "IXAudio2SourceVoice", NULL);
    for (int i = 0; i < ENG_SLOTS; i++) set_slot(eng_vtbl, i, dll, "IXAudio2", NULL);

    set_slot(base_vtbl, 0, dll, "GetVoiceDetails", v_GetVoiceDetails);
    set_slot(base_vtbl, 1, dll, "SetOutputVoices", v_SetOutputVoices);
    for (int i = 2; i <= 4; i++) set_slot(base_vtbl, i, dll, "voice-ok", ok);        /* effects */
    set_slot(base_vtbl, 5, dll, "GetEffectState", v_GetEffectState);
    for (int i = 6; i <= 11; i++) set_slot(base_vtbl, i, dll, "voice-ok", ok);       /* effect and filter params */
    set_slot(base_vtbl, 12, dll, "SetVolume", v_SetVolume);
    set_slot(base_vtbl, 13, dll, "GetVolume", v_GetVolume);
    set_slot(base_vtbl, 14, dll, "SetChannelVolumes", v_SetChannelVolumes);
    set_slot(base_vtbl, 15, dll, "GetChannelVolumes", v_GetChannelVolumes);
    for (int i = 16; i <= 17; i++) set_slot(base_vtbl, i, dll, "voice-ok", ok);      /* output matrix */
    set_slot(base_vtbl, 18, dll, "DestroyVoice", v_DestroyVoice);

    memcpy(src_vtbl, base_vtbl, sizeof base_vtbl);
    set_slot(src_vtbl, 19, dll, "Start", s_Start);
    set_slot(src_vtbl, 20, dll, "Stop", s_Stop);
    set_slot(src_vtbl, 21, dll, "SubmitSourceBuffer", s_SubmitSourceBuffer);
    set_slot(src_vtbl, 22, dll, "FlushSourceBuffers", s_FlushSourceBuffers);
    set_slot(src_vtbl, 23, dll, "Discontinuity", ok);
    set_slot(src_vtbl, 24, dll, "ExitLoop", ok);
    set_slot(src_vtbl, 25, dll, "GetState", s_GetState);
    set_slot(src_vtbl, 26, dll, "SetFrequencyRatio", s_SetFrequencyRatio);
    set_slot(src_vtbl, 27, dll, "GetFrequencyRatio", s_GetFrequencyRatio);

    set_slot(eng_vtbl, 0, dll, "QueryInterface", e_QueryInterface);
    set_slot(eng_vtbl, 1, dll, "AddRef", e_AddRef);
    set_slot(eng_vtbl, 2, dll, "Release", e_Release);
    set_slot(eng_vtbl, 3, dll, "GetDeviceCount", e_GetDeviceCount);
    set_slot(eng_vtbl, 4, dll, "GetDeviceDetails", e_NotImplemented);
    set_slot(eng_vtbl, 5, dll, "CreateSourceVoice", e_CreateSourceVoice);
    set_slot(eng_vtbl, 6, dll, "CreateSubmixVoice", e_CreateSubmixVoice);
    set_slot(eng_vtbl, 7, dll, "CreateMasteringVoice", e_CreateMasteringVoice);
    set_slot(eng_vtbl, 8, dll, "StartEngine", e_StartEngine);
    set_slot(eng_vtbl, 9, dll, "StopEngine", e_StopEngine);
    set_slot(eng_vtbl, 10, dll, "CommitChanges", ok);
    set_slot(eng_vtbl, 11, dll, "GetPerformanceData", ok);
    set_slot(eng_vtbl, 12, dll, "SetDebugConfiguration", ok);
}

/* HRESULT XAudio2Create(IXAudio2 **, UINT32 flags, XAUDIO2_PROCESSOR) */
static uint64_t xa_XAudio2Create(W64Ctx *c) {
    void **out = (void **)(uintptr_t)w64_arg(c, 0);
    if (!out) return HR_INVALIDARG;
    init_vtables();
    /* Each call makes an independent engine: its own output, mixer thread, and voices. */
    Engine *e = calloc(1, sizeof *e);
    if (!e) return HR_FAIL;
    e->vtbl = eng_vtbl;
    e->refs = 1;
    e->running = 1;
    e->master_gain = 1.0f;
    e->out = audio_out_open(OUT_RATE, OUT_CH);
    if (!e->out || pthread_create(&e->thr, NULL, mix_thread, e) != 0) {
        audio_out_close(e->out);
        free(e);
        return HR_FAIL;
    }
    *out = e;
    return HR_OK;
}

const HostExport xaudio2_exports[] = {
    { "XAudio2Create", xa_XAudio2Create },
    { NULL, NULL },
};

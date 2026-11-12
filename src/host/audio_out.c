#define _DEFAULT_SOURCE
#include "audio_out.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct AudioOut {
    snd_pcm_t *pcm;      /* NULL in null mode */
    FILE *dump;          /* optional raw dump */
    unsigned rate, channels;
};

static void pace(size_t frames, unsigned rate) {
    struct timespec ts;
    uint64_t ns = (uint64_t)frames * 1000000000ull / rate;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

AudioOut *audio_out_open(unsigned rate, unsigned channels) {
    AudioOut *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->rate = rate;
    o->channels = channels;

    const char *dump = getenv("XBEMU_AUDIO_DUMP");
    if (dump && *dump) o->dump = fopen(dump, "wb");

    const char *mode = getenv("XBEMU_AUDIO");
    if (!(mode && strcmp(mode, "null") == 0)) {
        int err = snd_pcm_open(&o->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
        if (err == 0) {
            err = snd_pcm_set_params(o->pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                     channels, rate, 1, 100000 /* 100 ms latency */);
            if (err != 0) { snd_pcm_close(o->pcm); o->pcm = NULL; }
        } else {
            o->pcm = NULL;
        }
        if (!o->pcm)
            fprintf(stderr, "[xbemu] audio: no ALSA playback device, using a silent sink\n");
    }
    return o;
}

int audio_out_write(AudioOut *o, const int16_t *frames, size_t nframes) {
    if (!o || !nframes) return 0;
    if (o->dump) fwrite(frames, sizeof(int16_t), nframes * o->channels, o->dump);
    if (!o->pcm) {
        pace(nframes, o->rate);
        return 0;
    }
    while (nframes) {
        snd_pcm_sframes_t n = snd_pcm_writei(o->pcm, frames, nframes);
        if (n < 0) {
            n = snd_pcm_recover(o->pcm, (int)n, 1);
            if (n < 0) return -1;
            continue;
        }
        frames += (size_t)n * o->channels;
        nframes -= (size_t)n;
    }
    return 0;
}

void audio_out_close(AudioOut *o) {
    if (!o) return;
    if (o->pcm) {
        snd_pcm_drain(o->pcm);
        snd_pcm_close(o->pcm);
    }
    if (o->dump) fclose(o->dump);
    free(o);
}

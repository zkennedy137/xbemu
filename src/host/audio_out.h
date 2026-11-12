#ifndef XBEMU_AUDIO_OUT_H
#define XBEMU_AUDIO_OUT_H

#include <stddef.h>
#include <stdint.h>

typedef struct AudioOut AudioOut;

/*
 * Open the host's audio output: ALSA "default" with 16-bit interleaved samples.
 * With no device (or XBEMU_AUDIO=null), frames are paced in real time and discarded.
 * XBEMU_AUDIO_DUMP=<path> also writes every frame to that file as raw S16LE.
 */
AudioOut *audio_out_open(unsigned rate, unsigned channels);

/* Write interleaved S16 frames. Blocks until the device accepts them. Returns 0 on success. */
int audio_out_write(AudioOut *o, const int16_t *frames, size_t nframes);

void audio_out_close(AudioOut *o);

#endif

#ifndef AUDIO_PCM_H
#define AUDIO_PCM_H

#include <soundio/soundio.h>

int pcm_sample_bytes(enum SoundIoFormat fmt);
float pcm_decode_sample(enum SoundIoFormat fmt, const unsigned char *p);
void pcm_encode_sample(enum SoundIoFormat fmt, unsigned char *p, float val);
void pcm_fade_frames(enum SoundIoFormat fmt, unsigned char *data, int nframes,
    int nchannels, int frame_bytes, int sample_bytes, int start_pos,
    int total_fade, int fade_out);

#endif /* AUDIO_PCM_H */

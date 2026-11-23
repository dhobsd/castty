#ifndef AUDIO_WRITER_M4A_H
#define AUDIO_WRITER_M4A_H

#include <stddef.h>
#include <stdio.h>

typedef void (*audio_m4a_sample_cb)(void *userdata, int src_rate, float sl,
    float sr);

struct audio_writer *audio_writer_m4a(FILE *outfile, int sample_rate,
    int nchannels, int mono);

int audio_m4a_probe(const char *path, int *rate_out, int *stereo_out,
    char *errbuf, size_t errsz);

int audio_m4a_decode(const char *path, audio_m4a_sample_cb cb, void *userdata,
    char *errbuf, size_t errsz);

#endif

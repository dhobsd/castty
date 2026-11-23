#ifndef AUDIO_WRITER_SNDFILE_H
#define AUDIO_WRITER_SNDFILE_H

#include <stdio.h>

#include "audio.h"

int audio_sndfile_format(enum audio_format fmt);
struct audio_writer *audio_writer_sndfile(FILE *outfile, enum audio_format fmt,
    int sample_rate, int nchannels, int buf_time_s, int mono);

#endif

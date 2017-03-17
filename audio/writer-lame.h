#ifndef AUDIO_WRITER_LAME_H
#define AUDIO_WRITER_LAME_H

#include "writer.h"

struct audio_writer *audio_writer_lame(FILE *outfile, int sample_rate, int nchannels,
    int buf_time_s, int mono);

#endif /* AUDIO_WRITER_LAME_H */

#ifndef AUDIO_WRITER_LAME_H
#define AUDIO_WRITER_LAME_H

#include "writer.h"

#ifdef WITH_LAME
struct audio_writer *audio_writer_lame(FILE *outfile, int sample_rate, int nchannels,
    int buf_time_s, int mono);
#else
#define audio_writer_lame(...) (NULL)
#endif

#endif /* AUDIO_WRITER_LAME_H */

#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>

enum audio_format {
	AUDIO_FMT_AUTO = 0,
	AUDIO_FMT_RAW,
	AUDIO_FMT_MP3,
	AUDIO_FMT_WAV,
	AUDIO_FMT_FLAC,
	AUDIO_FMT_OPUS,
	AUDIO_FMT_OGG,
	AUDIO_FMT_M4A,
};

double audio_clock_ms(void);
void audio_exit(void);
void audio_list_inputs(void);
void audio_mute(void);
int audio_set_format(const char *fmt_name);
int audio_check_append(const char *outfile, char *errbuf, size_t errsz);
void audio_init(const char *devid, const char *outfile, int use_raw);
void audio_init_append(const char *devid, const char *outfile, int use_raw,
    int append, double target_cast_ms);
void audio_start(void);
void audio_stop(void);
void audio_toggle_mp3(void);
void audio_toggle_mute(void);
void audio_toggle_pause(void);


#endif

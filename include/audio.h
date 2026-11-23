#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>

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

struct audio_vu {
	float rms_l;
	float rms_r;
	float peak_l;
	float peak_r;
	int clipped;
	int muted;
	int stereo;
	int active;
	uint64_t seq;
};

double audio_clock_ms(void);
void audio_get_vu(struct audio_vu *out);
int audio_is_muted(void);
const char *audio_active_format_name(void);
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

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

typedef void (*audio_pcm_cb)(void *userdata, int src_rate, float sl, float sr);

double audio_clock_ms(void);
uint64_t audio_vu_pack(float rms_l, float rms_r, float peak_l, float peak_r,
    int clipped, int stereo);
void audio_vu_unpack(uint64_t packed, struct audio_vu *out);
void audio_get_vu(struct audio_vu *out);
int audio_is_muted(void);
const char *audio_active_format_name(void);
void audio_exit(void);
void audio_list_inputs(void);
void audio_list_outputs(void);
void audio_mute(void);
int audio_set_format(const char *fmt_name);
enum audio_format audio_infer_format(const char *path);
const char *audio_format_label(enum audio_format afmt);
int audio_parse_format(const char *fmt_name, enum audio_format *out);
int audio_probe_file(const char *path, enum audio_format fmt, int *rate_out,
    int *stereo_out, char *errbuf, size_t errsz);
int audio_decode_file(const char *path, enum audio_format fmt,
    audio_pcm_cb cb, void *userdata, char *errbuf, size_t errsz);
struct audio_writer *audio_open_writer(FILE *fout, enum audio_format afmt,
    int rate, int mono);
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

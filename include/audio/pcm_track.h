#ifndef AUDIO_PCM_TRACK_H
#define AUDIO_PCM_TRACK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "audio.h"
#include "cast_doc.h"

enum {
	PCM_TRACK_RATE = 48000,
	PCM_TRACK_CHANNELS = 2,
	PCM_TRACK_FADE_FRAMES = 480, /* 10 ms half-cosine fade at 48 kHz */
	PCM_PEAK_BIN_FRAMES = 4800,  /* 100 ms peak summary bin */
};

struct pcm_piece {
	int buf_id; /* -1 = digital silence */
	uint64_t off_frames;
	uint64_t nframes;
	float gain;
	int fade_in;
	int fade_out;
	int mix_buf_id; /* -1 = none */
	uint64_t mix_off;
	float mix_gain;
};

struct pcm_edl_snapshot {
	struct pcm_piece *pieces;
	size_t npieces;
	uint64_t total_frames;
	int has_audio;
	int source_stereo;
};

struct pcm_track {
	FILE **files;
	size_t nfiles;
	uint64_t *file_frames;

	struct pcm_piece *pieces;
	size_t npieces;
	size_t cap;
	uint64_t total_frames;

	int source_stereo;
	enum audio_format source_fmt;
	int has_audio;

	float *peaks;
	size_t npeaks;
	uint64_t edl_gen;
	uint64_t peaks_gen;
};

struct pcm_audio_engine;

void pcm_track_init(struct pcm_track *tr);
void pcm_track_destroy(struct pcm_track *tr);

int pcm_track_load(struct pcm_track *tr, const char *path,
    enum audio_format fmt_hint, double min_dur_ms, char *errbuf, size_t errsz);
void pcm_track_create_empty(struct pcm_track *tr, double dur_ms);
int pcm_track_save(struct pcm_track *tr, const char *path,
    enum audio_format fmt, int mono, char *errbuf, size_t errsz);

int pcm_edl_snapshot_take(const struct pcm_track *tr,
    struct pcm_edl_snapshot *snap);
void pcm_edl_snapshot_free(struct pcm_edl_snapshot *snap);
int pcm_edl_snapshot_restore(struct pcm_track *tr,
    const struct pcm_edl_snapshot *snap);

double pcm_track_duration_ms(const struct pcm_track *tr);
int pcm_track_insert_silence(struct pcm_track *tr, double at_ms, double add_ms);
int pcm_track_cut_range(struct pcm_track *tr, double t0_ms, double t1_ms);
int pcm_track_cut_gaps(struct pcm_track *tr, const struct cast_cut_gap *gaps,
    size_t ngaps);
int pcm_track_mute_range(struct pcm_track *tr, double t0_ms, double t1_ms);
int pcm_track_nudge(struct pcm_track *tr, double shift_ms);
int pcm_track_apply_recording(struct pcm_track *tr, double at_ms, int buf_id,
    uint64_t off_frames, uint64_t nframes, int replace_mode, float duck_gain);
int pcm_track_new_scratch(struct pcm_track *tr, int *buf_id_out, FILE **fp_out);

size_t pcm_track_read_frames(struct pcm_track *tr, uint64_t start_frame,
    float *out_lr, size_t nframes);
void pcm_track_ensure_peaks(struct pcm_track *tr);
float pcm_track_peak_in_ms(const struct pcm_track *tr, double t0_ms,
    double t1_ms);

/* Live SoundIoOutStream playback + SoundIoInStream dubbing engine */
struct pcm_audio_engine *pcm_engine_create(struct pcm_track *tr,
    const char *in_devid, const char *out_devid, int use_raw);
void pcm_engine_destroy(struct pcm_audio_engine *eng);

int pcm_engine_start_play(struct pcm_audio_engine *eng, double start_ms,
    double speed, char *errbuf, size_t errsz);
void pcm_engine_stop_play(struct pcm_audio_engine *eng);
double pcm_engine_play_pos_ms(struct pcm_audio_engine *eng);
int pcm_engine_is_playing(struct pcm_audio_engine *eng);
void pcm_engine_set_play_mute(struct pcm_audio_engine *eng, int muted);
int pcm_engine_get_play_mute(const struct pcm_audio_engine *eng);

int pcm_engine_start_dub(struct pcm_audio_engine *eng, char *errbuf,
    size_t errsz);
int pcm_engine_stop_dub(struct pcm_audio_engine *eng, int *buf_id_out,
    uint64_t *nframes_out);
double pcm_engine_dub_ms(struct pcm_audio_engine *eng);

void pcm_engine_get_vu(struct pcm_audio_engine *eng, struct audio_vu *out);

#endif

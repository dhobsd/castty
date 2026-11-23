#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <soundio/soundio.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio/pcm.h"
#include "audio/pcm_track.h"
#include "audio/writer.h"

static enum SoundIoFormat sio_formats[] = {
	SoundIoFormatFloat32NE,
	SoundIoFormatFloat32FE,
	SoundIoFormatS32NE,
	SoundIoFormatS32FE,
	SoundIoFormatS24NE,
	SoundIoFormatS24FE,
	SoundIoFormatS16NE,
	SoundIoFormatS16FE,
	SoundIoFormatFloat64NE,
	SoundIoFormatFloat64FE,
	SoundIoFormatU32NE,
	SoundIoFormatU32FE,
	SoundIoFormatU24NE,
	SoundIoFormatU24FE,
	SoundIoFormatU16NE,
	SoundIoFormatU16FE,
	SoundIoFormatS8,
	SoundIoFormatU8,
	SoundIoFormatInvalid,
};

static int pref_rates[] = {
	48000,
	44100,
	24000,
	96000,
	0,
};

static uint64_t
ms_to_frames(double ms)
{
	if (ms <= 0.0) {
		return 0;
	}
	return (uint64_t)llround(ms * (double)PCM_TRACK_RATE / 1000.0);
}

static FILE *
open_unlinked_scratch(void)
{
	const char *tmp = getenv("TMPDIR");
	char path[512];
	int fd;
	FILE *fp;

	if (tmp == NULL || *tmp == '\0') {
		tmp = "/tmp";
	}
	snprintf(path, sizeof path, "%s/castty_ws_XXXXXX", tmp);
	fd = mkstemp(path);
	if (fd < 0) {
		return NULL;
	}
	(void)unlink(path);
	fp = fdopen(fd, "w+b");
	if (fp == NULL) {
		close(fd);
		return NULL;
	}
	return fp;
}

static float
clamp_unit(float v)
{
	if (v > 1.0f) {
		return 1.0f;
	}
	if (v < -1.0f) {
		return -1.0f;
	}
	return v;
}

static void
vu_from_stereo_chunk(const float *lr, size_t nframes, int stereo,
    atomic_uint_fast64_t *vu_packed, atomic_uint_fast64_t *vu_seq)
{
	double sum_l = 0.0, sum_r = 0.0;
	float max_l = 0.0f, max_r = 0.0f;
	int clip = 0;

	if (nframes == 0) {
		return;
	}
	for (size_t i = 0; i < nframes; i++) {
		float al = fabsf(lr[i * 2]);
		float ar = fabsf(lr[i * 2 + 1]);

		if (al > max_l) {
			max_l = al;
		}
		if (ar > max_r) {
			max_r = ar;
		}
		if (al >= 0.99f || ar >= 0.99f) {
			clip = 1;
		}
		sum_l += (double)lr[i * 2] * (double)lr[i * 2];
		sum_r += (double)lr[i * 2 + 1] * (double)lr[i * 2 + 1];
	}
	atomic_store_explicit(vu_packed,
	    audio_vu_pack(sqrtf((float)(sum_l / (double)nframes)),
		sqrtf((float)(sum_r / (double)nframes)), max_l, max_r, clip,
		stereo),
	    memory_order_relaxed);
	atomic_fetch_add_explicit(vu_seq, 1, memory_order_release);
}

void
pcm_track_init(struct pcm_track *tr)
{
	memset(tr, 0, sizeof *tr);
	tr->source_fmt = AUDIO_FMT_AUTO;
	tr->source_stereo = 1;
}

void
pcm_track_destroy(struct pcm_track *tr)
{
	if (tr == NULL) {
		return;
	}
	for (size_t i = 0; i < tr->nfiles; i++) {
		if (tr->files[i] != NULL) {
			fclose(tr->files[i]);
		}
	}
	free(tr->files);
	free(tr->file_frames);
	free(tr->pieces);
	free(tr->peaks);
	memset(tr, 0, sizeof *tr);
}

int
pcm_track_new_scratch(struct pcm_track *tr, int *buf_id_out, FILE **fp_out)
{
	FILE **nf, *fp;
	uint64_t *nfr;
	int idx;

	fp = open_unlinked_scratch();
	if (fp == NULL) {
		return -1;
	}
	nf = realloc(tr->files, (tr->nfiles + 1) * sizeof *nf);
	if (nf == NULL) {
		fclose(fp);
		return -1;
	}
	tr->files = nf;
	nfr = realloc(tr->file_frames, (tr->nfiles + 1) * sizeof *nfr);
	if (nfr == NULL) {
		fclose(fp);
		return -1;
	}
	tr->file_frames = nfr;

	idx = (int)tr->nfiles;
	tr->files[idx] = fp;
	tr->file_frames[idx] = 0;
	tr->nfiles++;
	if (buf_id_out != NULL) {
		*buf_id_out = idx;
	}
	if (fp_out != NULL) {
		*fp_out = fp;
	}
	return 0;
}

static void
edl_recompute_total(struct pcm_track *tr)
{
	uint64_t tot = 0;

	for (size_t i = 0; i < tr->npieces; i++) {
		tot += tr->pieces[i].nframes;
	}
	tr->total_frames = tot;
	tr->edl_gen++;
}

static int
edl_ensure_cap(struct pcm_track *tr, size_t need)
{
	if (need <= tr->cap) {
		return 0;
	}
	size_t nc = tr->cap ? tr->cap * 2 : 32;
	while (nc < need) {
		nc *= 2;
	}
	struct pcm_piece *np = realloc(tr->pieces, nc * sizeof *np);

	if (np == NULL) {
		return -1;
	}
	tr->pieces = np;
	tr->cap = nc;
	return 0;
}

static int
edl_insert_piece(struct pcm_track *tr, size_t idx, const struct pcm_piece *p)
{
	if (p->nframes == 0) {
		return 0;
	}
	if (edl_ensure_cap(tr, tr->npieces + 1) != 0) {
		return -1;
	}
	if (idx < tr->npieces) {
		memmove(&tr->pieces[idx + 1], &tr->pieces[idx],
		    (tr->npieces - idx) * sizeof *tr->pieces);
	}
	tr->pieces[idx] = *p;
	tr->npieces++;
	return 0;
}

/* Split piece containing frame_pos cleanly, returning index of piece starting at
 * frame_pos (or npieces if frame_pos >= total_frames). */
static int
edl_split_at(struct pcm_track *tr, uint64_t frame_pos, size_t *idx_out)
{
	uint64_t acc = 0;

	for (size_t i = 0; i < tr->npieces; i++) {
		struct pcm_piece *p = &tr->pieces[i];

		if (frame_pos == acc) {
			*idx_out = i;
			return 0;
		}
		if (frame_pos < acc + p->nframes) {
			uint64_t off = frame_pos - acc;
			struct pcm_piece right = *p;

			right.off_frames = (p->buf_id >= 0) ?
			    (p->off_frames + off) : 0;
			right.mix_off = (p->mix_buf_id >= 0) ?
			    (p->mix_off + off) : 0;
			right.nframes = p->nframes - off;
			right.fade_in = 0;

			p->nframes = off;
			p->fade_out = 0;
			if (edl_insert_piece(tr, i + 1, &right) != 0) {
				return -1;
			}
			*idx_out = i + 1;
			return 0;
		}
		acc += p->nframes;
	}
	*idx_out = tr->npieces;
	return 0;
}

static int
edl_ensure_frames(struct pcm_track *tr, uint64_t want_frames)
{
	if (want_frames <= tr->total_frames) {
		return 0;
	}
	struct pcm_piece sil = {
		.buf_id = -1,
		.nframes = want_frames - tr->total_frames,
		.gain = 1.0f,
		.mix_buf_id = -1,
	};
	if (edl_insert_piece(tr, tr->npieces, &sil) != 0) {
		return -1;
	}
	edl_recompute_total(tr);
	return 0;
}

struct load_resampler {
	FILE *fp;
	float chunk[4096 * 2];
	size_t chunk_len;
	uint64_t frames;
	int src_rate;
	int have_prev;
	float prev_l;
	float prev_r;
	double phase;
	double step;
	int io_err;
};

static void
load_flush_chunk(struct load_resampler *lr)
{
	if (lr->chunk_len == 0 || lr->io_err) {
		return;
	}
	if (fwrite(lr->chunk, sizeof(float) * 2, lr->chunk_len, lr->fp) !=
	    lr->chunk_len) {
		lr->io_err = 1;
	}
	lr->frames += lr->chunk_len;
	lr->chunk_len = 0;
}

static void
load_push_dst(struct load_resampler *lr, float sl, float sr)
{
	lr->chunk[lr->chunk_len * 2] = sl;
	lr->chunk[lr->chunk_len * 2 + 1] = sr;
	lr->chunk_len++;
	if (lr->chunk_len >= 4096) {
		load_flush_chunk(lr);
	}
}

static void
load_pcm_cb(void *userdata, int src_rate, float sl, float sr)
{
	struct load_resampler *lr = userdata;

	if (lr->io_err) {
		return;
	}
	if (lr->src_rate == 0 && src_rate > 0) {
		lr->src_rate = src_rate;
		lr->step = (double)src_rate / (double)PCM_TRACK_RATE;
	}
	if (lr->src_rate == PCM_TRACK_RATE) {
		load_push_dst(lr, sl, sr);
		return;
	}
	if (!lr->have_prev) {
		lr->prev_l = sl;
		lr->prev_r = sr;
		lr->have_prev = 1;
		lr->phase = 0.0;
		return;
	}
	while (lr->phase < 1.0) {
		double f = lr->phase;

		load_push_dst(lr,
		    (float)(lr->prev_l + f * (sl - lr->prev_l)),
		    (float)(lr->prev_r + f * (sr - lr->prev_r)));
		lr->phase += lr->step;
	}
	lr->phase -= 1.0;
	lr->prev_l = sl;
	lr->prev_r = sr;
}

int
pcm_track_load(struct pcm_track *tr, const char *path,
    enum audio_format fmt_hint, double min_dur_ms, char *errbuf, size_t errsz)
{
	enum audio_format fmt = (fmt_hint != AUDIO_FMT_AUTO) ? fmt_hint :
	    audio_infer_format(path);
	int rate = 0, stereo = 1, buf_id = -1;
	FILE *fp = NULL;
	struct load_resampler lr;

	pcm_track_destroy(tr);
	pcm_track_init(tr);

	if (fmt == AUDIO_FMT_RAW) {
		FILE *rf = fopen(path, "rb");
		float buf[4096 * 2];
		size_t nr;

		if (rf == NULL) {
			snprintf(errbuf, errsz, "castty: cannot open %s: %s",
			    path, strerror(errno));
			return -1;
		}
		if (pcm_track_new_scratch(tr, &buf_id, &fp) != 0) {
			fclose(rf);
			snprintf(errbuf, errsz, "castty: scratch file failed");
			return -1;
		}
		while ((nr = fread(buf, sizeof(float) * 2, 4096, rf)) > 0) {
			if (fwrite(buf, sizeof(float) * 2, nr, fp) != nr) {
				fclose(rf);
				pcm_track_destroy(tr);
				snprintf(errbuf, errsz,
				    "castty: scratch write failed");
				return -1;
			}
			tr->file_frames[buf_id] += nr;
		}
		fclose(rf);
		tr->source_fmt = AUDIO_FMT_RAW;
		tr->source_stereo = 1;
	} else {
		if (audio_probe_file(path, fmt, &rate, &stereo, errbuf,
		    errsz) != 0) {
			return -1;
		}
		if (pcm_track_new_scratch(tr, &buf_id, &fp) != 0) {
			snprintf(errbuf, errsz, "castty: scratch file failed");
			return -1;
		}
		memset(&lr, 0, sizeof lr);
		lr.fp = fp;
		if (audio_decode_file(path, fmt, load_pcm_cb, &lr, errbuf,
		    errsz) != 0 || lr.io_err) {
			if (lr.io_err) {
				snprintf(errbuf, errsz,
				    "castty: scratch write failed");
			}
			pcm_track_destroy(tr);
			return -1;
		}
		if (lr.src_rate != PCM_TRACK_RATE && lr.have_prev) {
			load_push_dst(&lr, lr.prev_l, lr.prev_r);
		}
		load_flush_chunk(&lr);
		tr->file_frames[buf_id] = lr.frames;
		tr->source_fmt = fmt;
		tr->source_stereo = stereo;
	}

	if (fflush(fp) != 0) {
		pcm_track_destroy(tr);
		snprintf(errbuf, errsz, "castty: scratch flush failed");
		return -1;
	}

	if (tr->file_frames[buf_id] > 0) {
		struct pcm_piece p = {
			.buf_id = buf_id,
			.off_frames = 0,
			.nframes = tr->file_frames[buf_id],
			.gain = 1.0f,
			.mix_buf_id = -1,
		};

		if (edl_insert_piece(tr, 0, &p) != 0) {
			pcm_track_destroy(tr);
			snprintf(errbuf, errsz, "castty: out of memory");
			return -1;
		}
	}
	edl_recompute_total(tr);
	tr->has_audio = 1;

	uint64_t min_frames = ms_to_frames(min_dur_ms);
	if (min_frames > tr->total_frames) {
		(void)edl_ensure_frames(tr, min_frames);
	}
	return 0;
}

void
pcm_track_create_empty(struct pcm_track *tr, double dur_ms)
{
	pcm_track_destroy(tr);
	pcm_track_init(tr);
	tr->has_audio = 0;
	tr->source_stereo = 1;
	(void)edl_ensure_frames(tr, ms_to_frames(dur_ms));
}

int
pcm_edl_snapshot_take(const struct pcm_track *tr, struct pcm_edl_snapshot *snap)
{
	memset(snap, 0, sizeof *snap);
	if (tr->npieces > 0) {
		snap->pieces = malloc(tr->npieces * sizeof *snap->pieces);
		if (snap->pieces == NULL) {
			return -1;
		}
		memcpy(snap->pieces, tr->pieces,
		    tr->npieces * sizeof *snap->pieces);
	}
	snap->npieces = tr->npieces;
	snap->total_frames = tr->total_frames;
	snap->has_audio = tr->has_audio;
	snap->source_stereo = tr->source_stereo;
	return 0;
}

void
pcm_edl_snapshot_free(struct pcm_edl_snapshot *snap)
{
	if (snap == NULL) {
		return;
	}
	free(snap->pieces);
	memset(snap, 0, sizeof *snap);
}

int
pcm_edl_snapshot_restore(struct pcm_track *tr,
    const struct pcm_edl_snapshot *snap)
{
	if (snap->npieces > tr->cap &&
	    edl_ensure_cap(tr, snap->npieces) != 0) {
		return -1;
	}
	if (snap->npieces > 0) {
		memcpy(tr->pieces, snap->pieces,
		    snap->npieces * sizeof *tr->pieces);
	}
	tr->npieces = snap->npieces;
	tr->total_frames = snap->total_frames;
	tr->has_audio = snap->has_audio;
	tr->source_stereo = snap->source_stereo;
	tr->edl_gen++;
	return 0;
}

double
pcm_track_duration_ms(const struct pcm_track *tr)
{
	return ((double)tr->total_frames * 1000.0) / (double)PCM_TRACK_RATE;
}

static void
edl_apply_fade_out(struct pcm_piece *p)
{
	int fn = PCM_TRACK_FADE_FRAMES;

	if (p->buf_id < 0) {
		return;
	}
	if (p->nframes < (uint64_t)fn) {
		fn = (int)p->nframes;
	}
	p->fade_out = fn;
}

static void
edl_apply_fade_in(struct pcm_piece *p)
{
	int fn = PCM_TRACK_FADE_FRAMES;

	if (p->buf_id < 0) {
		return;
	}
	if (p->nframes < (uint64_t)fn) {
		fn = (int)p->nframes;
	}
	p->fade_in = fn;
}

int
pcm_track_insert_silence(struct pcm_track *tr, double at_ms, double add_ms)
{
	uint64_t f0 = ms_to_frames(at_ms);
	uint64_t add_f = ms_to_frames(add_ms);
	size_t i0 = 0;

	if (add_f == 0) {
		return 0;
	}
	if (f0 > tr->total_frames) {
		if (edl_ensure_frames(tr, f0) != 0) {
			return -1;
		}
	}
	uint64_t acc = 0;
	for (size_t i = 0; i < tr->npieces; i++) {
		struct pcm_piece *p = &tr->pieces[i];

		if (f0 >= acc && f0 <= acc + p->nframes &&
		    p->buf_id < 0 && p->mix_buf_id < 0) {
			p->nframes += add_f;
			edl_recompute_total(tr);
			return 0;
		}
		acc += p->nframes;
	}
	if (edl_split_at(tr, f0, &i0) != 0) {
		return -1;
	}
	struct pcm_piece sil = {
		.buf_id = -1,
		.nframes = add_f,
		.gain = 1.0f,
		.mix_buf_id = -1,
	};
	if (edl_insert_piece(tr, i0, &sil) != 0) {
		return -1;
	}
	if (i0 > 0) {
		edl_apply_fade_out(&tr->pieces[i0 - 1]);
	}
	if (i0 + 1 < tr->npieces) {
		edl_apply_fade_in(&tr->pieces[i0 + 1]);
	}
	edl_recompute_total(tr);
	return 0;
}

int
pcm_track_cut_range(struct pcm_track *tr, double t0_ms, double t1_ms)
{
	uint64_t f0 = ms_to_frames(t0_ms);
	uint64_t f1 = ms_to_frames(t1_ms);
	size_t i0 = 0, i1 = 0;

	if (f0 >= tr->total_frames || f1 <= f0) {
		return 0;
	}
	if (f1 > tr->total_frames) {
		f1 = tr->total_frames;
	}
	if (edl_split_at(tr, f0, &i0) != 0 ||
	    edl_split_at(tr, f1, &i1) != 0) {
		return -1;
	}
	if (i1 > i0) {
		size_t rem = tr->npieces - i1;

		if (rem > 0) {
			memmove(&tr->pieces[i0], &tr->pieces[i1],
			    rem * sizeof *tr->pieces);
		}
		tr->npieces -= (i1 - i0);
	}
	if (i0 > 0) {
		edl_apply_fade_out(&tr->pieces[i0 - 1]);
	}
	if (i0 < tr->npieces) {
		edl_apply_fade_in(&tr->pieces[i0]);
	}
	edl_recompute_total(tr);
	return 0;
}

int
pcm_track_cut_gaps(struct pcm_track *tr, const struct cast_cut_gap *gaps,
    size_t ngaps)
{
	for (size_t i = 0; i < ngaps; i++) {
		if (pcm_track_cut_range(tr, gaps[i].t0_ms, gaps[i].t1_ms) != 0) {
			return -1;
		}
	}
	return 0;
}

int
pcm_track_mute_range(struct pcm_track *tr, double t0_ms, double t1_ms)
{
	uint64_t f0 = ms_to_frames(t0_ms);
	uint64_t f1 = ms_to_frames(t1_ms);
	size_t i0 = 0, i1 = 0;

	if (f0 >= tr->total_frames || f1 <= f0) {
		return 0;
	}
	if (f1 > tr->total_frames) {
		f1 = tr->total_frames;
	}
	if (edl_split_at(tr, f0, &i0) != 0 ||
	    edl_split_at(tr, f1, &i1) != 0) {
		return -1;
	}
	if (i1 > i0) {
		size_t rem = tr->npieces - i1;

		if (rem > 0) {
			memmove(&tr->pieces[i0], &tr->pieces[i1],
			    rem * sizeof *tr->pieces);
		}
		tr->npieces -= (i1 - i0);
	}
	struct pcm_piece sil = {
		.buf_id = -1,
		.nframes = f1 - f0,
		.gain = 1.0f,
		.mix_buf_id = -1,
	};
	if (edl_insert_piece(tr, i0, &sil) != 0) {
		return -1;
	}
	if (i0 > 0) {
		edl_apply_fade_out(&tr->pieces[i0 - 1]);
	}
	if (i0 + 1 < tr->npieces) {
		edl_apply_fade_in(&tr->pieces[i0 + 1]);
	}
	edl_recompute_total(tr);
	return 0;
}

int
pcm_track_nudge(struct pcm_track *tr, double shift_ms)
{
	if (shift_ms > 0.0) {
		return pcm_track_insert_silence(tr, 0.0, shift_ms);
	}
	if (shift_ms < 0.0) {
		return pcm_track_cut_range(tr, 0.0, -shift_ms);
	}
	return 0;
}

static void
read_scratch_frames(FILE *fp, uint64_t off_frames, float *buf, size_t nframes)
{
	size_t want = nframes * 2 * sizeof(float);
	size_t got = 0;
	off_t off = (off_t)(off_frames * 2 * sizeof(float));
	int fd;

	if (want == 0) {
		return;
	}
	if (fp == NULL) {
		memset(buf, 0, want);
		return;
	}
	fd = fileno(fp);
	while (got < want) {
		ssize_t nr = pread(fd, (char *)buf + got, want - got,
		    off + (off_t)got);

		if (nr < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (nr == 0) {
			break;
		}
		got += (size_t)nr;
	}
	if (got < want) {
		memset((char *)buf + got, 0, want - got);
	}
}

size_t
pcm_track_read_frames(struct pcm_track *tr, uint64_t start_frame,
    float *out_lr, size_t nframes)
{
	size_t written = 0;
	uint64_t acc = 0;
	float tmp[1024 * 2], mtmp[1024 * 2];

	memset(out_lr, 0, nframes * 2 * sizeof(float));
	if (start_frame >= tr->total_frames || nframes == 0) {
		return 0;
	}

	for (size_t i = 0; i < tr->npieces && written < nframes; i++) {
		const struct pcm_piece *p = &tr->pieces[i];
		uint64_t p_end = acc + p->nframes;
		uint64_t cur = start_frame + written;

		if (cur >= p_end) {
			acc = p_end;
			continue;
		}
		uint64_t p_off = cur - acc;
		uint64_t take = p_end - cur;

		if (take > nframes - written) {
			take = nframes - written;
		}
		while (take > 0) {
			size_t chunk = (take > 1024) ? 1024 : (size_t)take;
			float *dst = out_lr + written * 2;

			if (p->buf_id >= 0 && (size_t)p->buf_id < tr->nfiles) {
				read_scratch_frames(tr->files[p->buf_id],
				    p->off_frames + p_off, tmp, chunk);
				for (size_t k = 0; k < chunk; k++) {
					uint64_t pos = p_off + k;
					float g = p->gain;

					if (p->fade_in > 0 &&
					    pos < (uint64_t)p->fade_in) {
						g *= (float)pcm_half_cosine_gain(
						    pos, (uint64_t)p->fade_in,
						    0);
					}
					if (p->fade_out > 0 &&
					    pos + (uint64_t)p->fade_out >= p->nframes) {
						uint64_t fpos = pos +
						    (uint64_t)p->fade_out -
						    p->nframes;

						g *= (float)pcm_half_cosine_gain(
						    fpos, (uint64_t)p->fade_out,
						    1);
					}
					dst[k * 2] = tmp[k * 2] * g;
					dst[k * 2 + 1] = tmp[k * 2 + 1] * g;
				}
			} else {
				memset(dst, 0, chunk * 2 * sizeof(float));
			}

			if (p->mix_buf_id >= 0 &&
			    (size_t)p->mix_buf_id < tr->nfiles) {
				read_scratch_frames(tr->files[p->mix_buf_id],
				    p->mix_off + p_off, mtmp, chunk);
				for (size_t k = 0; k < chunk; k++) {
					dst[k * 2] = clamp_unit(dst[k * 2] +
					    mtmp[k * 2] * p->mix_gain);
					dst[k * 2 + 1] = clamp_unit(
					    dst[k * 2 + 1] +
					    mtmp[k * 2 + 1] * p->mix_gain);
				}
			}

			written += chunk;
			p_off += chunk;
			take -= chunk;
		}
		acc = p_end;
	}
	return written;
}

static int
bake_range_into_scratch(struct pcm_track *tr, uint64_t f0, uint64_t f1,
    int *buf_out)
{
	FILE *fp = NULL;
	int buf_id = -1;
	float buf[4096 * 2];
	uint64_t pos = f0;

	if (pcm_track_new_scratch(tr, &buf_id, &fp) != 0) {
		return -1;
	}
	while (pos < f1) {
		size_t chunk = (f1 - pos > 4096) ? 4096 : (size_t)(f1 - pos);

		(void)pcm_track_read_frames(tr, pos, buf, chunk);
		if (fwrite(buf, 2 * sizeof(float), chunk, fp) != chunk) {
			return -1;
		}
		tr->file_frames[buf_id] += chunk;
		pos += chunk;
	}
	if (fflush(fp) != 0) {
		return -1;
	}
	*buf_out = buf_id;
	return 0;
}

int
pcm_track_apply_recording(struct pcm_track *tr, double at_ms, int buf_id,
    uint64_t off_frames, uint64_t nframes, int replace_mode, float duck_gain)
{
	uint64_t f0 = ms_to_frames(at_ms);
	uint64_t f1 = f0 + nframes;
	size_t i0 = 0, i1 = 0;

	if (nframes == 0 || buf_id < 0 || (size_t)buf_id >= tr->nfiles) {
		return 0;
	}
	if (f1 > tr->total_frames && edl_ensure_frames(tr, f1) != 0) {
		return -1;
	}
	if (edl_split_at(tr, f0, &i0) != 0 ||
	    edl_split_at(tr, f1, &i1) != 0) {
		return -1;
	}

	if (replace_mode || !tr->has_audio) {
		if (i1 > i0) {
			size_t rem = tr->npieces - i1;

			if (rem > 0) {
				memmove(&tr->pieces[i0], &tr->pieces[i1],
				    rem * sizeof *tr->pieces);
			}
			tr->npieces -= (i1 - i0);
		}
		int fn = PCM_TRACK_FADE_FRAMES;
		if (nframes < (uint64_t)fn) {
			fn = (int)nframes;
		}
		struct pcm_piece rec = {
			.buf_id = buf_id,
			.off_frames = off_frames,
			.nframes = nframes,
			.gain = 1.0f,
			.fade_in = fn,
			.fade_out = fn,
			.mix_buf_id = -1,
		};
		if (edl_insert_piece(tr, i0, &rec) != 0) {
			return -1;
		}
		if (i0 > 0) {
			edl_apply_fade_out(&tr->pieces[i0 - 1]);
		}
		if (i0 + 1 < tr->npieces) {
			edl_apply_fade_in(&tr->pieces[i0 + 1]);
		}
	} else {
		/* If any piece inside [i0, i1) already has a mix overlay, bake
		 * [f0, f1) into one clean bed piece before applying the new pass. */
		int need_bake = 0;

		for (size_t k = i0; k < i1; k++) {
			if (tr->pieces[k].mix_buf_id >= 0) {
				need_bake = 1;
				break;
			}
		}
		if (need_bake) {
			int baked_id = -1;

			if (bake_range_into_scratch(tr, f0, f1, &baked_id) != 0) {
				return -1;
			}
			size_t rem = tr->npieces - i1;

			if (rem > 0) {
				memmove(&tr->pieces[i0], &tr->pieces[i1],
				    rem * sizeof *tr->pieces);
			}
			tr->npieces -= (i1 - i0);
			struct pcm_piece bed = {
				.buf_id = baked_id,
				.off_frames = 0,
				.nframes = nframes,
				.gain = duck_gain,
				.mix_buf_id = buf_id,
				.mix_off = off_frames,
				.mix_gain = 1.0f,
			};
			if (edl_insert_piece(tr, i0, &bed) != 0) {
				return -1;
			}
		} else {
			uint64_t acc = 0;

			for (size_t k = i0; k < i1; k++) {
				tr->pieces[k].gain *= duck_gain;
				tr->pieces[k].mix_buf_id = buf_id;
				tr->pieces[k].mix_off = off_frames + acc;
				tr->pieces[k].mix_gain = 1.0f;
				acc += tr->pieces[k].nframes;
			}
		}
	}

	tr->has_audio = 1;
	edl_recompute_total(tr);
	return 0;
}

void
pcm_track_ensure_peaks(struct pcm_track *tr)
{
	size_t bins;
	float chunk[PCM_PEAK_BIN_FRAMES * 2];

	if (tr->peaks_gen == tr->edl_gen && tr->peaks != NULL) {
		return;
	}
	bins = (size_t)((tr->total_frames + PCM_PEAK_BIN_FRAMES - 1) /
	    PCM_PEAK_BIN_FRAMES);
	free(tr->peaks);
	tr->peaks = NULL;
	tr->npeaks = 0;
	if (bins == 0) {
		tr->peaks_gen = tr->edl_gen;
		return;
	}
	tr->peaks = calloc(bins, sizeof *tr->peaks);
	if (tr->peaks == NULL) {
		return;
	}
	tr->npeaks = bins;
	if (!tr->has_audio) {
		tr->peaks_gen = tr->edl_gen;
		return;
	}
	for (size_t b = 0; b < bins; b++) {
		uint64_t f0 = (uint64_t)b * PCM_PEAK_BIN_FRAMES;
		size_t got = pcm_track_read_frames(tr, f0, chunk,
		    PCM_PEAK_BIN_FRAMES);
		float pk = 0.0f;

		for (size_t i = 0; i < got * 2; i++) {
			float a = fabsf(chunk[i]);

			if (a > pk) {
				pk = a;
			}
		}
		tr->peaks[b] = pk;
	}
	tr->peaks_gen = tr->edl_gen;
}

float
pcm_track_peak_in_ms(const struct pcm_track *tr, double t0_ms, double t1_ms)
{
	uint64_t f0, f1;
	size_t b0, b1;
	float pk = 0.0f;

	if (tr->npeaks == 0 || tr->peaks == NULL) {
		return 0.0f;
	}
	f0 = ms_to_frames(t0_ms);
	f1 = ms_to_frames(t1_ms);
	b0 = (size_t)(f0 / PCM_PEAK_BIN_FRAMES);
	if (b0 >= tr->npeaks) {
		return 0.0f;
	}
	b1 = (size_t)((f1 + PCM_PEAK_BIN_FRAMES - 1) / PCM_PEAK_BIN_FRAMES);
	if (b1 <= b0) {
		b1 = b0 + 1;
	}
	if (b1 > tr->npeaks) {
		b1 = tr->npeaks;
	}
	for (size_t b = b0; b < b1; b++) {
		if (tr->peaks[b] > pk) {
			pk = tr->peaks[b];
		}
	}
	return pk;
}

int
pcm_track_save(struct pcm_track *tr, const char *path, enum audio_format fmt,
    int mono, char *errbuf, size_t errsz)
{
	size_t tlen = strlen(path) + sizeof(".castty.XXXXXX");
	char *tmp = malloc(tlen);
	int fd;
	FILE *fp;
	struct audio_writer *aw;
	float buf[4096 * 2];
	uint64_t pos = 0;

	if (fmt == AUDIO_FMT_AUTO) {
		fmt = audio_infer_format(path);
	}
	if (tmp == NULL) {
		snprintf(errbuf, errsz, "castty: out of memory saving %s",
		    path);
		return -1;
	}
	snprintf(tmp, tlen, "%s.castty.XXXXXX", path);
	fd = mkstemp(tmp);
	if (fd < 0) {
		snprintf(errbuf, errsz, "castty: cannot create temp for %s: %s",
		    path, strerror(errno));
		free(tmp);
		return -1;
	}
	(void)fchmod(fd, 0644);
	fp = fdopen(fd, "w+b");
	if (fp == NULL) {
		snprintf(errbuf, errsz, "castty: fdopen failed: %s",
		    strerror(errno));
		close(fd);
		unlink(tmp);
		free(tmp);
		return -1;
	}

	aw = audio_open_writer(fp, fmt, PCM_TRACK_RATE, mono);
	if (aw == NULL) {
		snprintf(errbuf, errsz, "castty: couldn't open audio writer");
		fclose(fp);
		unlink(tmp);
		free(tmp);
		return -1;
	}

	while (pos < tr->total_frames) {
		size_t chunk = (tr->total_frames - pos > 4096) ? 4096 :
		    (size_t)(tr->total_frames - pos);

		(void)pcm_track_read_frames(tr, pos, buf, chunk);
		audio_writer_write(aw, SoundIoFormatFloat32NE, (char *)buf,
		    (int)(chunk * 2 * sizeof(float)),
		    2 * (int)sizeof(float));
		pos += chunk;
	}
	audio_writer_destroy(aw);
	if (fclose(fp) != 0) {
		snprintf(errbuf, errsz, "castty: failed closing %s", tmp);
		unlink(tmp);
		free(tmp);
		return -1;
	}
	if (rename(tmp, path) != 0) {
		snprintf(errbuf, errsz, "castty: rename failed for %s: %s",
		    path, strerror(errno));
		unlink(tmp);
		free(tmp);
		return -1;
	}
	free(tmp);
	return 0;
}

struct pcm_vu {
	atomic_uint_fast64_t packed;
	atomic_uint_fast64_t seq;
};

struct pcm_play_rt {
	struct SoundIoRingBuffer *rb;
	atomic_uint_fast64_t played_frames;
	atomic_int active;
	atomic_int eof;
	atomic_int muted;
	int frame_bytes;
	int sample_bytes;
};

struct pcm_play_ctrl {
	struct SoundIo *io;
	struct SoundIoDevice *dev;
	struct SoundIoOutStream *stream;
	pthread_t thread;
	double start_ms;
	double speed;
	enum SoundIoFormat fmt;
	int rate;
	int thread_started;
	atomic_int stop_req;
};

struct pcm_dub_rt {
	struct SoundIoRingBuffer *rb;
	atomic_int active;
	int frame_bytes;
	int sample_bytes;
	int mono;
};

struct pcm_dub_ctrl {
	atomic_uint_fast64_t frames_written;
	FILE *fp;
	struct SoundIo *io;
	struct SoundIoDevice *dev;
	struct SoundIoInStream *stream;
	pthread_t thread;
	enum SoundIoFormat fmt;
	int rate;
	int buf_id;
	int thread_started;
	atomic_int stop_req;
};

struct pcm_audio_engine {
	struct pcm_track *tr;
	char *in_devid;
	char *out_devid;
	double last_play_ms;
	int use_raw;

	_Alignas(64) struct pcm_play_rt play_rt;
	_Alignas(64) struct pcm_dub_rt dub_rt;
	_Alignas(64) struct pcm_vu vu;
	_Alignas(64) struct pcm_play_ctrl play;
	_Alignas(64) struct pcm_dub_ctrl dub;
};

static struct SoundIoDevice *
pick_device(struct SoundIo *io, const char *devid, int use_raw, int is_input)
{
	int count = is_input ? soundio_input_device_count(io) :
	    soundio_output_device_count(io);
	int def_idx;

	if (devid != NULL && *devid != '\0') {
		for (int i = 0; i < count; i++) {
			struct SoundIoDevice *d = is_input ?
			    soundio_get_input_device(io, i) :
			    soundio_get_output_device(io, i);

			if (strcmp(d->id, devid) == 0 && d->is_raw == use_raw) {
				return d;
			}
			soundio_device_unref(d);
		}
		return NULL;
	}

	def_idx = is_input ? soundio_default_input_device_index(io) :
	    soundio_default_output_device_index(io);
	if (def_idx < 0 || def_idx >= count) {
		return NULL;
	}
	return is_input ? soundio_get_input_device(io, def_idx) :
	    soundio_get_output_device(io, def_idx);
}

static int
open_audio_device(const char *devid, int use_raw, int is_input,
    struct SoundIo **io_out, struct SoundIoDevice **dev_out, char *errbuf,
    size_t errsz)
{
	struct SoundIo *io = soundio_create();
	struct SoundIoDevice *dev;
	int err;

	if (io == NULL) {
		snprintf(errbuf, errsz, "soundio_create failed");
		return -1;
	}
	if ((err = soundio_connect(io)) != 0) {
		snprintf(errbuf, errsz, "soundio_connect: %s",
		    soundio_strerror(err));
		soundio_destroy(io);
		return -1;
	}
	soundio_flush_events(io);

	dev = pick_device(io, devid, use_raw, is_input);
	if (dev == NULL || dev->probe_error) {
		snprintf(errbuf, errsz, "No usable audio %s device",
		    is_input ? "input" : "output");
		if (dev != NULL) {
			soundio_device_unref(dev);
		}
		soundio_disconnect(io);
		soundio_destroy(io);
		return -1;
	}
	soundio_device_sort_channel_layouts(dev);
	*io_out = io;
	*dev_out = dev;
	return 0;
}

static int
pick_stream_params(struct SoundIoDevice *dev, enum SoundIoFormat *fmt_out,
    int *rate_out, char *errbuf, size_t errsz)
{
	unsigned i;

	for (i = 0;; i++) {
		if (sio_formats[i] == SoundIoFormatInvalid) {
			snprintf(errbuf, errsz, "No usable audio format");
			return -1;
		}
		if (soundio_device_supports_format(dev, sio_formats[i])) {
			*fmt_out = sio_formats[i];
			break;
		}
	}
	for (i = 0;; i++) {
		if (pref_rates[i] == 0) {
			*rate_out = dev->sample_rate_current;
			break;
		}
		if (soundio_device_supports_sample_rate(dev, pref_rates[i])) {
			*rate_out = pref_rates[i];
			break;
		}
	}
	return 0;
}

static void
out_write_callback(struct SoundIoOutStream *stream, int frame_count_min,
    int frame_count_max)
{
	struct pcm_play_rt *rt = stream->userdata;
	struct SoundIoChannelArea *areas;
	int frame_bytes = rt->frame_bytes;
	int sample_bytes = rt->sample_bytes;
	int is_muted = atomic_load_explicit(&rt->muted, memory_order_relaxed);
	int avail_frames = soundio_ring_buffer_fill_count(rt->rb) / frame_bytes;
	const char *rptr = soundio_ring_buffer_read_ptr(rt->rb);
	int remaining = frame_count_max;
	int read_frames = 0;

	(void)frame_count_min;
	while (remaining > 0) {
		int nframe = remaining;
		int err;

		if ((err = soundio_outstream_begin_write(stream, &areas,
		    &nframe)) != 0) {
			return;
		}
		if (!nframe) {
			break;
		}
		if (areas == NULL) {
			(void)soundio_outstream_end_write(stream);
			break;
		}

		int copy_n = (avail_frames > nframe) ? nframe : avail_frames;

		for (int f = 0; f < copy_n; f++) {
			if (is_muted) {
				memset(areas[0].ptr, 0, (size_t)sample_bytes);
				if (stream->layout.channel_count > 1) {
					memset(areas[1].ptr, 0,
					    (size_t)sample_bytes);
				}
			} else {
				memcpy(areas[0].ptr, rptr,
				    (size_t)sample_bytes);
				if (stream->layout.channel_count > 1) {
					memcpy(areas[1].ptr,
					    rptr + sample_bytes,
					    (size_t)sample_bytes);
				}
			}
			rptr += frame_bytes;
			areas[0].ptr += areas[0].step;
			if (stream->layout.channel_count > 1) {
				areas[1].ptr += areas[1].step;
			}
		}
		for (int f = copy_n; f < nframe; f++) {
			memset(areas[0].ptr, 0, (size_t)sample_bytes);
			areas[0].ptr += areas[0].step;
			if (stream->layout.channel_count > 1) {
				memset(areas[1].ptr, 0, (size_t)sample_bytes);
				areas[1].ptr += areas[1].step;
			}
		}

		avail_frames -= copy_n;
		read_frames += copy_n;
		remaining -= nframe;

		if ((err = soundio_outstream_end_write(stream)) != 0) {
			return;
		}
	}

	if (read_frames > 0) {
		soundio_ring_buffer_advance_read_ptr(rt->rb,
		    read_frames * frame_bytes);
		atomic_fetch_add_explicit(&rt->played_frames,
		    (uint_fast64_t)read_frames, memory_order_release);
	} else if (atomic_load_explicit(&rt->eof, memory_order_acquire)) {
		atomic_store_explicit(&rt->active, 0, memory_order_release);
	}
}

static void *
play_feeder_main(void *arg)
{
	struct pcm_audio_engine *eng = arg;
	uint64_t src_pos = ms_to_frames(eng->play.start_ms);
	double step = ((double)PCM_TRACK_RATE * eng->play.speed) /
	    (double)eng->play.rate;
	double phase = 0.0;
	float src_buf[1024 * 2];
	size_t src_fill = 0, src_idx = 0;
	float vu_lr[512 * 2];
	unsigned char out_bytes[512 * 16];
	int is_eof = 0;
	int sample_bytes = eng->play_rt.sample_bytes;
	int frame_bytes = eng->play_rt.frame_bytes;
	int lead_bytes = (eng->play.rate / 5) * frame_bytes;

	while (!atomic_load_explicit(&eng->play.stop_req,
	    memory_order_acquire)) {
		int fill = soundio_ring_buffer_fill_count(eng->play_rt.rb);
		int freec = soundio_ring_buffer_free_count(eng->play_rt.rb);

		if (is_eof) {
			if (fill < frame_bytes) {
				break;
			}
			struct timespec ts = { 0, 10000000L };

			nanosleep(&ts, NULL);
			continue;
		}
		if (fill >= lead_bytes || freec < frame_bytes * 64) {
			struct timespec ts = { 0, 5000000L };

			nanosleep(&ts, NULL);
			continue;
		}

		int max_out = freec / frame_bytes;
		if (max_out > 512) {
			max_out = 512;
		}
		int produced = 0;

		while (produced < max_out) {
			if (src_idx + 1 >= src_fill) {
				if (src_idx < src_fill) {
					src_buf[0] = src_buf[src_idx * 2];
					src_buf[1] = src_buf[src_idx * 2 + 1];
					src_fill = 1;
				} else {
					src_fill = 0;
				}
				src_idx = 0;
				size_t got = pcm_track_read_frames(eng->tr,
				    src_pos, src_buf + src_fill * 2,
				    1024 - src_fill);

				src_pos += got;
				src_fill += got;
				if (src_fill < 2) {
					is_eof = 1;
					break;
				}
			}

			float l0 = src_buf[src_idx * 2];
			float r0 = src_buf[src_idx * 2 + 1];
			float l1 = src_buf[(src_idx + 1) * 2];
			float r1 = src_buf[(src_idx + 1) * 2 + 1];
			float sl = (float)(l0 + phase * (l1 - l0));
			float sr = (float)(r0 + phase * (r1 - r0));

			vu_lr[produced * 2] = sl;
			vu_lr[produced * 2 + 1] = sr;
			pcm_encode_sample(eng->play.fmt,
			    out_bytes + (size_t)produced * frame_bytes,
			    sl * 32768.0f);
			pcm_encode_sample(eng->play.fmt,
			    out_bytes + (size_t)produced * frame_bytes +
			    sample_bytes, sr * 32768.0f);
			produced++;

			phase += step;
			while (phase >= 1.0 && src_idx + 1 < src_fill) {
				phase -= 1.0;
				src_idx++;
			}
		}

		if (produced > 0) {
			char *wptr = soundio_ring_buffer_write_ptr(
			    eng->play_rt.rb);

			memcpy(wptr, out_bytes, (size_t)produced * frame_bytes);
			soundio_ring_buffer_advance_write_ptr(eng->play_rt.rb,
			    produced * frame_bytes);
			vu_from_stereo_chunk(vu_lr, (size_t)produced,
			    eng->tr->source_stereo, &eng->vu.packed,
			    &eng->vu.seq);
		}
		if (is_eof) {
			atomic_store_explicit(&eng->play_rt.eof, 1,
			    memory_order_release);
		}
	}

	atomic_store_explicit(&eng->play_rt.eof, 1, memory_order_release);
	return NULL;
}

struct pcm_audio_engine *
pcm_engine_create(struct pcm_track *tr, const char *in_devid,
    const char *out_devid, int use_raw)
{
	void *mem = NULL;
	struct pcm_audio_engine *eng;

	if (posix_memalign(&mem, 64, sizeof *eng) != 0 || mem == NULL) {
		return NULL;
	}
	eng = mem;
	memset(eng, 0, sizeof *eng);
	eng->tr = tr;
	eng->in_devid = in_devid ? strdup(in_devid) : NULL;
	eng->out_devid = out_devid ? strdup(out_devid) : NULL;
	eng->use_raw = use_raw;
	eng->dub.buf_id = -1;
	atomic_init(&eng->play_rt.played_frames, 0);
	atomic_init(&eng->play_rt.active, 0);
	atomic_init(&eng->play_rt.eof, 0);
	atomic_init(&eng->play_rt.muted, 0);
	atomic_init(&eng->play.stop_req, 0);
	atomic_init(&eng->dub_rt.active, 0);
	atomic_init(&eng->dub.frames_written, 0);
	atomic_init(&eng->dub.stop_req, 0);
	atomic_init(&eng->vu.packed, 0);
	atomic_init(&eng->vu.seq, 0);
	return eng;
}

void
pcm_engine_destroy(struct pcm_audio_engine *eng)
{
	if (eng == NULL) {
		return;
	}
	pcm_engine_stop_play(eng);
	(void)pcm_engine_stop_dub(eng, NULL, NULL);
	free(eng->in_devid);
	free(eng->out_devid);
	free(eng);
}

int
pcm_engine_start_play(struct pcm_audio_engine *eng, double start_ms,
    double speed, char *errbuf, size_t errsz)
{
	const struct SoundIoChannelLayout *stereo;
	int err;

	pcm_engine_stop_play(eng);
	pcm_track_ensure_peaks(eng->tr);
	if (speed <= 0.0) {
		speed = 1.0;
	}
	eng->last_play_ms = start_ms;
	eng->play.start_ms = start_ms;
	eng->play.speed = speed;
	atomic_store_explicit(&eng->play.stop_req, 0, memory_order_release);
	atomic_store_explicit(&eng->play_rt.played_frames, 0,
	    memory_order_release);
	atomic_store_explicit(&eng->play_rt.active, 0, memory_order_release);
	atomic_store_explicit(&eng->play_rt.eof, 0, memory_order_release);
	atomic_store_explicit(&eng->vu.packed, 0, memory_order_relaxed);

	if (open_audio_device(eng->out_devid, eng->use_raw, 0, &eng->play.io,
	    &eng->play.dev, errbuf, errsz) != 0) {
		pcm_engine_stop_play(eng);
		return -1;
	}

	eng->play.stream = soundio_outstream_create(eng->play.dev);
	if (eng->play.stream == NULL) {
		snprintf(errbuf, errsz, "soundio_outstream_create failed");
		pcm_engine_stop_play(eng);
		return -1;
	}
	stereo = soundio_channel_layout_get_builtin(SoundIoChannelLayoutIdStereo);
	eng->play.stream->layout = soundio_device_supports_layout(eng->play.dev,
	    stereo) ? *stereo : eng->play.dev->layouts[0];

	if (pick_stream_params(eng->play.dev, &eng->play.stream->format,
	    &eng->play.stream->sample_rate, errbuf, errsz) != 0) {
		pcm_engine_stop_play(eng);
		return -1;
	}
	eng->play.stream->write_callback = out_write_callback;
	eng->play.stream->userdata = &eng->play_rt;

	if ((err = soundio_outstream_open(eng->play.stream)) != 0) {
		snprintf(errbuf, errsz, "soundio_outstream_open: %s",
		    soundio_strerror(err));
		pcm_engine_stop_play(eng);
		return -1;
	}

	eng->play.fmt = eng->play.stream->format;
	eng->play.rate = eng->play.stream->sample_rate;
	eng->play_rt.sample_bytes = eng->play.stream->bytes_per_sample;
	eng->play_rt.frame_bytes = 2 * eng->play_rt.sample_bytes;
	eng->play_rt.rb = soundio_ring_buffer_create(eng->play.io,
	    2 * eng->play.rate * eng->play_rt.frame_bytes);
	if (eng->play_rt.rb == NULL) {
		snprintf(errbuf, errsz, "ring buffer allocation failed");
		pcm_engine_stop_play(eng);
		return -1;
	}

	atomic_store_explicit(&eng->play_rt.active, 1, memory_order_release);
	if (pthread_create(&eng->play.thread, NULL, play_feeder_main, eng) != 0) {
		atomic_store_explicit(&eng->play_rt.active, 0,
		    memory_order_release);
		snprintf(errbuf, errsz, "pthread_create failed");
		pcm_engine_stop_play(eng);
		return -1;
	}
	eng->play.thread_started = 1;

	/* Wait briefly (~20ms) so feeder pre-fills before audio callback fires. */
	for (int t = 0; t < 10; t++) {
		if (soundio_ring_buffer_fill_count(eng->play_rt.rb) >=
		    eng->play_rt.frame_bytes * 512 ||
		    atomic_load_explicit(&eng->play_rt.eof,
		    memory_order_acquire)) {
			break;
		}
		struct timespec ts = { 0, 2000000L };

		nanosleep(&ts, NULL);
	}

	if ((err = soundio_outstream_start(eng->play.stream)) != 0) {
		snprintf(errbuf, errsz, "soundio_outstream_start: %s",
		    soundio_strerror(err));
		pcm_engine_stop_play(eng);
		return -1;
	}
	return 0;
}

void
pcm_engine_stop_play(struct pcm_audio_engine *eng)
{
	if (eng == NULL) {
		return;
	}
	if (eng->play.rate > 0) {
		eng->last_play_ms = pcm_engine_play_pos_ms(eng);
	}
	atomic_store_explicit(&eng->play.stop_req, 1, memory_order_release);
	atomic_store_explicit(&eng->play_rt.active, 0, memory_order_release);
	if (eng->play.stream != NULL) {
		soundio_outstream_destroy(eng->play.stream);
		eng->play.stream = NULL;
	}
	if (eng->play.thread_started) {
		pthread_join(eng->play.thread, NULL);
		eng->play.thread_started = 0;
	}
	if (eng->play_rt.rb != NULL) {
		soundio_ring_buffer_destroy(eng->play_rt.rb);
		eng->play_rt.rb = NULL;
	}
	if (eng->play.dev != NULL) {
		soundio_device_unref(eng->play.dev);
		eng->play.dev = NULL;
	}
	if (eng->play.io != NULL) {
		soundio_disconnect(eng->play.io);
		soundio_destroy(eng->play.io);
		eng->play.io = NULL;
	}
	eng->play.rate = 0;
	atomic_store_explicit(&eng->play_rt.eof, 0, memory_order_relaxed);
	atomic_store_explicit(&eng->play_rt.played_frames, 0,
	    memory_order_relaxed);
	atomic_store_explicit(&eng->vu.packed, 0, memory_order_relaxed);
}

double
pcm_engine_play_pos_ms(struct pcm_audio_engine *eng)
{
	uint64_t fr;
	double elapsed, tot;

	if (eng == NULL) {
		return 0.0;
	}
	if (eng->play.rate <= 0) {
		return eng->last_play_ms;
	}
	fr = atomic_load_explicit(&eng->play_rt.played_frames,
	    memory_order_acquire);
	elapsed = ((double)fr * 1000.0 / (double)eng->play.rate) *
	    eng->play.speed;
	tot = pcm_track_duration_ms(eng->tr);
	return (eng->play.start_ms + elapsed > tot) ? tot :
	    (eng->play.start_ms + elapsed);
}

int
pcm_engine_is_playing(struct pcm_audio_engine *eng)
{
	if (eng == NULL) {
		return 0;
	}
	return atomic_load_explicit(&eng->play_rt.active, memory_order_acquire);
}

void
pcm_engine_set_play_mute(struct pcm_audio_engine *eng, int muted)
{
	atomic_store_explicit(&eng->play_rt.muted, muted ? 1 : 0,
	    memory_order_relaxed);
}

int
pcm_engine_get_play_mute(const struct pcm_audio_engine *eng)
{
	return atomic_load_explicit(&eng->play_rt.muted, memory_order_relaxed);
}

static void
dub_read_callback(struct SoundIoInStream *stream, int min_frames, int max_frames)
{
	struct pcm_dub_rt *rt = stream->userdata;
	struct SoundIoChannelArea *areas;
	int frame_bytes = rt->frame_bytes;
	int sample_bytes = rt->sample_bytes;
	int is_rec = atomic_load_explicit(&rt->active, memory_order_acquire);
	char *buf = soundio_ring_buffer_write_ptr(rt->rb);
	int nfree = soundio_ring_buffer_free_count(rt->rb) / frame_bytes;
	int to_write = (nfree < max_frames) ? nfree : max_frames;
	int remaining = to_write;
	int written = 0;

	if (!is_rec || nfree < min_frames) {
		int rem = max_frames;

		while (rem > 0 && rem >= min_frames) {
			int nf = rem;

			if (soundio_instream_begin_read(stream, &areas, &nf) != 0 ||
			    !nf) {
				break;
			}
			(void)soundio_instream_end_read(stream);
			rem -= nf;
		}
		return;
	}

	while (remaining > 0 && remaining >= min_frames) {
		int nf = remaining;

		if (soundio_instream_begin_read(stream, &areas, &nf) != 0 ||
		    !nf) {
			break;
		}
		if (!areas) {
			memset(buf, 0, (size_t)nf * frame_bytes);
			buf += (size_t)nf * frame_bytes;
		} else {
			for (int f = 0; f < nf; f++) {
				memcpy(buf, areas[0].ptr, sample_bytes);
				buf += sample_bytes;
				if (rt->mono) {
					memcpy(buf, areas[0].ptr, sample_bytes);
				} else {
					memcpy(buf, areas[1].ptr, sample_bytes);
					areas[1].ptr += areas[1].step;
				}
				buf += sample_bytes;
				areas[0].ptr += areas[0].step;
			}
		}
		written += nf;
		remaining -= nf;
		if (soundio_instream_end_read(stream) != 0) {
			break;
		}
	}

	if (written > 0) {
		soundio_ring_buffer_advance_write_ptr(rt->rb,
		    written * frame_bytes);
	}
}

static void *
dub_writer_main(void *arg)
{
	struct pcm_audio_engine *eng = arg;
	int frame_bytes = eng->dub_rt.frame_bytes;
	int sample_bytes = eng->dub_rt.sample_bytes;
	struct load_resampler lr;
	float vu_lr[1024 * 2];

	memset(&lr, 0, sizeof lr);
	lr.fp = eng->dub.fp;
	lr.src_rate = eng->dub.rate;
	lr.step = (double)eng->dub.rate / (double)PCM_TRACK_RATE;

	for (;;) {
		int stop = atomic_load_explicit(&eng->dub.stop_req,
		    memory_order_acquire);
		int fill = soundio_ring_buffer_fill_count(eng->dub_rt.rb);
		int nf = fill / frame_bytes;

		if (nf > 0) {
			const unsigned char *rptr = (const unsigned char *)
			    soundio_ring_buffer_read_ptr(eng->dub_rt.rb);
			int take = (nf > 1024) ? 1024 : nf;
			uint64_t before = lr.frames + lr.chunk_len;

			for (int i = 0; i < take; i++) {
				const unsigned char *fr = rptr +
				    (size_t)i * frame_bytes;
				float sl = pcm_decode_sample(eng->dub.fmt,
				    fr) / 32768.0f;
				float sr = pcm_decode_sample(eng->dub.fmt,
				    fr + sample_bytes) / 32768.0f;

				vu_lr[i * 2] = sl;
				vu_lr[i * 2 + 1] = sr;
				load_pcm_cb(&lr, eng->dub.rate, sl, sr);
			}
			soundio_ring_buffer_advance_read_ptr(eng->dub_rt.rb,
			    take * frame_bytes);
			vu_from_stereo_chunk(vu_lr, (size_t)take,
			    !eng->dub_rt.mono, &eng->vu.packed, &eng->vu.seq);
			uint64_t after = lr.frames + lr.chunk_len;

			if (after > before) {
				atomic_fetch_add_explicit(
				    &eng->dub.frames_written, after - before,
				    memory_order_release);
			}
			continue;
		}
		if (stop) {
			break;
		}
		struct timespec ts = { 0, 5000000L };

		nanosleep(&ts, NULL);
	}

	if (lr.src_rate != PCM_TRACK_RATE && lr.have_prev) {
		uint64_t b = lr.frames + lr.chunk_len;

		load_push_dst(&lr, lr.prev_l, lr.prev_r);
		if (lr.frames + lr.chunk_len > b) {
			atomic_fetch_add_explicit(&eng->dub.frames_written,
			    (lr.frames + lr.chunk_len) - b,
			    memory_order_release);
		}
	}
	load_flush_chunk(&lr);
	fflush(eng->dub.fp);
	return NULL;
}

int
pcm_engine_start_dub(struct pcm_audio_engine *eng, char *errbuf, size_t errsz)
{
	const struct SoundIoChannelLayout *stereo, *mono;
	int err;

	pcm_engine_stop_play(eng);
	(void)pcm_engine_stop_dub(eng, NULL, NULL);

	eng->dub.buf_id = -1;
	eng->dub.fp = NULL;
	atomic_store_explicit(&eng->dub.frames_written, 0,
	    memory_order_release);
	atomic_store_explicit(&eng->dub_rt.active, 0, memory_order_release);
	atomic_store_explicit(&eng->dub.stop_req, 0, memory_order_release);
	atomic_store_explicit(&eng->vu.packed, 0, memory_order_relaxed);

	if (pcm_track_new_scratch(eng->tr, &eng->dub.buf_id,
	    &eng->dub.fp) != 0) {
		snprintf(errbuf, errsz, "couldn't allocate scratch recording");
		return -1;
	}

	if (open_audio_device(eng->in_devid, eng->use_raw, 1, &eng->dub.io,
	    &eng->dub.dev, errbuf, errsz) != 0) {
		(void)pcm_engine_stop_dub(eng, NULL, NULL);
		return -1;
	}

	eng->dub.stream = soundio_instream_create(eng->dub.dev);
	if (eng->dub.stream == NULL) {
		snprintf(errbuf, errsz, "soundio_instream_create failed");
		(void)pcm_engine_stop_dub(eng, NULL, NULL);
		return -1;
	}
	stereo = soundio_channel_layout_get_builtin(SoundIoChannelLayoutIdStereo);
	mono = soundio_channel_layout_get_builtin(SoundIoChannelLayoutIdMono);
	if (soundio_device_supports_layout(eng->dub.dev, stereo)) {
		eng->dub.stream->layout = *stereo;
		eng->dub_rt.mono = 0;
	} else if (soundio_device_supports_layout(eng->dub.dev, mono)) {
		eng->dub.stream->layout = *mono;
		eng->dub_rt.mono = 1;
	} else {
		eng->dub.stream->layout = eng->dub.dev->layouts[0];
		eng->dub_rt.mono = (eng->dub.stream->layout.channel_count == 1);
	}

	if (pick_stream_params(eng->dub.dev, &eng->dub.stream->format,
	    &eng->dub.stream->sample_rate, errbuf, errsz) != 0) {
		(void)pcm_engine_stop_dub(eng, NULL, NULL);
		return -1;
	}

	eng->dub.stream->read_callback = dub_read_callback;
	eng->dub.stream->userdata = &eng->dub_rt;
	if ((err = soundio_instream_open(eng->dub.stream)) != 0) {
		snprintf(errbuf, errsz, "soundio_instream_open: %s",
		    soundio_strerror(err));
		(void)pcm_engine_stop_dub(eng, NULL, NULL);
		return -1;
	}

	eng->dub.fmt = eng->dub.stream->format;
	eng->dub.rate = eng->dub.stream->sample_rate;
	eng->dub_rt.sample_bytes = eng->dub.stream->bytes_per_sample;
	eng->dub_rt.frame_bytes = 2 * eng->dub_rt.sample_bytes;
	eng->dub_rt.rb = soundio_ring_buffer_create(eng->dub.io,
	    2 * eng->dub.rate * eng->dub_rt.frame_bytes);
	if (eng->dub_rt.rb == NULL) {
		snprintf(errbuf, errsz, "dub ring buffer allocation failed");
		(void)pcm_engine_stop_dub(eng, NULL, NULL);
		return -1;
	}

	atomic_store_explicit(&eng->dub_rt.active, 1, memory_order_release);
	if (pthread_create(&eng->dub.thread, NULL, dub_writer_main, eng) != 0) {
		atomic_store_explicit(&eng->dub_rt.active, 0,
		    memory_order_release);
		snprintf(errbuf, errsz, "pthread_create failed");
		(void)pcm_engine_stop_dub(eng, NULL, NULL);
		return -1;
	}
	eng->dub.thread_started = 1;

	if ((err = soundio_instream_start(eng->dub.stream)) != 0) {
		snprintf(errbuf, errsz, "soundio_instream_start: %s",
		    soundio_strerror(err));
		(void)pcm_engine_stop_dub(eng, NULL, NULL);
		return -1;
	}
	return 0;
}

int
pcm_engine_stop_dub(struct pcm_audio_engine *eng, int *buf_id_out,
    uint64_t *nframes_out)
{
	int id;
	uint64_t nf;

	if (eng == NULL) {
		return -1;
	}
	atomic_store_explicit(&eng->dub.stop_req, 1, memory_order_release);
	atomic_store_explicit(&eng->dub_rt.active, 0, memory_order_release);
	if (eng->dub.stream != NULL) {
		soundio_instream_destroy(eng->dub.stream);
		eng->dub.stream = NULL;
	}
	if (eng->dub.thread_started) {
		pthread_join(eng->dub.thread, NULL);
		eng->dub.thread_started = 0;
	}
	if (eng->dub_rt.rb != NULL) {
		soundio_ring_buffer_destroy(eng->dub_rt.rb);
		eng->dub_rt.rb = NULL;
	}
	if (eng->dub.dev != NULL) {
		soundio_device_unref(eng->dub.dev);
		eng->dub.dev = NULL;
	}
	if (eng->dub.io != NULL) {
		soundio_disconnect(eng->dub.io);
		soundio_destroy(eng->dub.io);
		eng->dub.io = NULL;
	}
	id = eng->dub.buf_id;
	nf = atomic_load_explicit(&eng->dub.frames_written,
	    memory_order_acquire);
	if (id >= 0 && (size_t)id < eng->tr->nfiles) {
		eng->tr->file_frames[id] = nf;
	}
	eng->dub.buf_id = -1;
	eng->dub.fp = NULL;
	atomic_store_explicit(&eng->vu.packed, 0, memory_order_relaxed);

	if (buf_id_out != NULL) {
		*buf_id_out = id;
	}
	if (nframes_out != NULL) {
		*nframes_out = nf;
	}
	return (id >= 0 && nf > 0) ? 0 : -1;
}

double
pcm_engine_dub_ms(struct pcm_audio_engine *eng)
{
	uint64_t nf;

	if (eng == NULL) {
		return 0.0;
	}
	nf = atomic_load_explicit(&eng->dub.frames_written,
	    memory_order_acquire);
	return ((double)nf * 1000.0) / (double)PCM_TRACK_RATE;
}

void
pcm_engine_get_vu(struct pcm_audio_engine *eng, struct audio_vu *out)
{
	if (out == NULL) {
		return;
	}
	memset(out, 0, sizeof *out);
	if (eng == NULL) {
		return;
	}
	int pl = atomic_load_explicit(&eng->play_rt.active,
	    memory_order_acquire);
	int db = atomic_load_explicit(&eng->dub_rt.active,
	    memory_order_acquire);

	out->active = pl || db;
	out->muted = pl ? atomic_load_explicit(&eng->play_rt.muted,
	    memory_order_relaxed) : 0;
	if (!out->active) {
		return;
	}
	out->seq = atomic_load_explicit(&eng->vu.seq, memory_order_acquire);
	audio_vu_unpack(atomic_load_explicit(&eng->vu.packed,
	    memory_order_relaxed), out);
}

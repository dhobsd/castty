#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <pthread.h>
#include <unistd.h>

#include <lame/lame.h>
#include <sndfile.h>
#include <soundio/soundio.h>

#include "audio.h"
#include "castty.h"
#include "audio/pcm.h"
#include "audio/writer.h"
#include "audio/writer-lame.h"
#include "audio/writer-raw.h"
#include "audio/writer-sndfile.h"

static enum SoundIoFormat formats[] = {
	SoundIoFormatFloat32NE,
	SoundIoFormatFloat32FE,
	SoundIoFormatFloat64NE,
	SoundIoFormatFloat64FE,
	SoundIoFormatS32NE,
	SoundIoFormatS32FE,
	SoundIoFormatU32NE,
	SoundIoFormatU32FE,
	SoundIoFormatS24NE,
	SoundIoFormatS24FE,
	SoundIoFormatU24NE,
	SoundIoFormatU24FE,
	SoundIoFormatS16NE,
	SoundIoFormatS16FE,
	SoundIoFormatU16NE,
	SoundIoFormatU16FE,
	SoundIoFormatS8,
	SoundIoFormatU8,
	SoundIoFormatInvalid,
};

enum {
	BUF_TIME_S = 10,
};

static int rates[] = {
	44100,
	24000,
	48000,
	96000,
	128000,
	0,
};

static int opus_rates[] = {
	48000,
	24000,
	16000,
	12000,
	8000,
	0,
};

static int
is_opus_rate(int rate)
{
	for (unsigned i = 0; opus_rates[i] != 0; i++) {
		if (opus_rates[i] == rate) {
			return 1;
		}
	}
	return 0;
}

static struct audio_ctx {
	const char *devid;
	char *outpath;
	char *tmp_path;
	atomic_uint_fast64_t clock_frames;
	double base_clock_ms;
	double target_cast_ms;
	atomic_int sample_rate;
	enum SoundIoFormat format;
	int bytes_per_sample;
	int frame_bytes;
	FILE *fout;
	struct audio_writer *aw;
	enum audio_format afmt;
	int aw_rate;
	int fade_in_total;
	int fade_in_pos;
	int active;
	int started;
	int prepared;
	int append;
	int existing_stereo;
	int pref_rate;
	int connected;
	int mono;
	int use_raw;

	struct SoundIoInStream *stream;
	struct SoundIoRingBuffer *rb;
	struct SoundIoDevice *dev;
	struct SoundIo *io;
} ctx;

static atomic_int recording;
static atomic_int stopping;
static atomic_int muted;
static atomic_int overflowed;
static atomic_int read_err;
static enum audio_format requested_fmt = AUDIO_FMT_AUTO;

static pthread_t wthread;

static const struct {
	enum audio_format afmt;
	const char *name;
	const char *ext;
} audio_fmt_table[] = {
	{ AUDIO_FMT_RAW,  "raw",  ".raw"  },
	{ AUDIO_FMT_MP3,  "mp3",  ".mp3"  },
	{ AUDIO_FMT_WAV,  "wav",  ".wav"  },
	{ AUDIO_FMT_FLAC, "flac", ".flac" },
	{ AUDIO_FMT_OPUS, "opus", ".opus" },
	{ AUDIO_FMT_OGG,  "ogg",  ".ogg"  },
};

static const char *
audio_format_name(enum audio_format afmt)
{
	for (unsigned i = 0;
	    i < sizeof audio_fmt_table / sizeof audio_fmt_table[0]; i++) {
		if (audio_fmt_table[i].afmt == afmt) {
			return audio_fmt_table[i].name;
		}
	}
	return "auto";
}

static enum audio_format
resolve_audio_format(const char *outfile)
{
	const char *slash, *base, *dot;

	if (requested_fmt != AUDIO_FMT_AUTO) {
		return requested_fmt;
	}
	if (outfile == NULL) {
		return AUDIO_FMT_RAW;
	}

	slash = strrchr(outfile, '/');
	base = slash ? slash + 1 : outfile;
	dot = strrchr(base, '.');
	if (dot == NULL) {
		return AUDIO_FMT_RAW;
	}
	for (unsigned i = 0;
	    i < sizeof audio_fmt_table / sizeof audio_fmt_table[0]; i++) {
		if (strcasecmp(dot, audio_fmt_table[i].ext) == 0) {
			return audio_fmt_table[i].afmt;
		}
	}
	return AUDIO_FMT_RAW;
}

static struct audio_writer *
create_audio_writer(FILE *fout, enum audio_format afmt, int rate, int mono)
{
	switch (afmt) {
	case AUDIO_FMT_RAW:
		return audio_writer_raw(fout);
	case AUDIO_FMT_MP3:
		return audio_writer_lame(fout, rate, 2, BUF_TIME_S, mono);
	case AUDIO_FMT_WAV:
	case AUDIO_FMT_FLAC:
	case AUDIO_FMT_OPUS:
	case AUDIO_FMT_OGG:
		return audio_writer_sndfile(fout, afmt, rate, 2, BUF_TIME_S,
		    mono);
	case AUDIO_FMT_AUTO:
		break;
	}
	return NULL;
}

int
audio_set_format(const char *fmt_name)
{
	if (fmt_name == NULL) {
		return -1;
	}
	if (strcasecmp(fmt_name, "auto") == 0) {
		requested_fmt = AUDIO_FMT_AUTO;
		return 0;
	}
	for (unsigned i = 0;
	    i < sizeof audio_fmt_table / sizeof audio_fmt_table[0]; i++) {
		if (strcasecmp(fmt_name, audio_fmt_table[i].name) == 0) {
			requested_fmt = audio_fmt_table[i].afmt;
			return 0;
		}
	}
	return -1;
}

static void
audio_teardown_backend(void)
{
	if (ctx.stream) {
		soundio_instream_destroy(ctx.stream);
		ctx.stream = NULL;
	}
	if (ctx.rb) {
		soundio_ring_buffer_destroy(ctx.rb);
		ctx.rb = NULL;
	}
	if (ctx.dev) {
		soundio_device_unref(ctx.dev);
		ctx.dev = NULL;
	}
	if (ctx.io) {
		if (ctx.connected) {
			soundio_disconnect(ctx.io);
			ctx.connected = 0;
		}
		soundio_destroy(ctx.io);
		ctx.io = NULL;
	}
}

static void
write_zero_frames(struct audio_writer *aw, enum SoundIoFormat fmt,
    int frame_bytes, uint64_t nframes)
{
	enum {
		ZERO_CHUNK_FRAMES = 4096,
	};
	unsigned char *zbuf;
	size_t chunk_bytes = (size_t)ZERO_CHUNK_FRAMES * (size_t)frame_bytes;
	int sample_bytes = pcm_sample_bytes(fmt);

	if (nframes == 0) {
		return;
	}
	zbuf = calloc(1, chunk_bytes);
	if (zbuf == NULL) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	/* Replicate an encoded 0.0 frame so unsigned formats stay at midscale silence. */
	if (sample_bytes > 0 && frame_bytes >= sample_bytes &&
	    frame_bytes % sample_bytes == 0) {
		int nch = frame_bytes / sample_bytes;
		int ch;

		for (ch = 0; ch < nch; ch++) {
			pcm_encode_sample(fmt, zbuf + (size_t)ch * (size_t)sample_bytes,
			    0.0f);
		}
		for (int i = 1; i < ZERO_CHUNK_FRAMES; i++) {
			memcpy(zbuf + (size_t)i * (size_t)frame_bytes, zbuf,
			    (size_t)frame_bytes);
		}
	}
	while (nframes > 0) {
		uint64_t chunk = nframes > ZERO_CHUNK_FRAMES ?
		    ZERO_CHUNK_FRAMES : nframes;

		audio_writer_write(aw, fmt, (char *)zbuf,
		    (int)(chunk * (uint64_t)frame_bytes), frame_bytes);
		nframes -= chunk;
	}
	free(zbuf);
}

static uint64_t
pad_audio_to_target(enum SoundIoFormat fmt, int frame_bytes, int rate,
    uint64_t pre_frames)
{
	if (ctx.target_cast_ms > 0.0 && rate > 0) {
		double cur_ms = ((double)pre_frames * 1000.0) / (double)rate;

		if (cur_ms < ctx.target_cast_ms) {
			uint64_t want = (uint64_t)llround(ctx.target_cast_ms *
			    (double)rate / 1000.0);

			if (want > pre_frames) {
				write_zero_frames(ctx.aw, fmt, frame_bytes,
				    want - pre_frames);
				pre_frames = want;
			}
		}
	}
	return pre_frames;
}

static int
probe_sndfile_file(const char *path, enum audio_format afmt, int *rate_out,
    int *stereo_out, char *errbuf, size_t errsz)
{
	SF_INFO info;
	SNDFILE *sf;
	int expected_sf, got_major, exp_major;

	expected_sf = audio_sndfile_format(afmt);
	memset(&info, 0, sizeof info);
	sf = sf_open(path, SFM_READ, &info);
	if (sf == NULL) {
		snprintf(errbuf, errsz, "castty: %s is not a valid %s file: %s",
		    path, audio_format_name(afmt), sf_strerror(NULL));
		return -1;
	}
	sf_close(sf);

	got_major = info.format & SF_FORMAT_TYPEMASK;
	exp_major = expected_sf & SF_FORMAT_TYPEMASK;
	if (got_major != exp_major ||
	    (exp_major == SF_FORMAT_OGG &&
	    (info.format & SF_FORMAT_SUBMASK) != (expected_sf & SF_FORMAT_SUBMASK))) {
		snprintf(errbuf, errsz,
		    "castty: %s is not in %s format", path,
		    audio_format_name(afmt));
		return -1;
	}
	if (info.samplerate <= 0 || info.channels <= 0 || info.frames <= 0) {
		snprintf(errbuf, errsz,
		    "castty: %s contains no valid audio frames", path);
		return -1;
	}

	if (rate_out) {
		*rate_out = info.samplerate;
	}
	if (stereo_out) {
		*stereo_out = (info.channels >= 2);
	}
	return 0;
}

static int
probe_mp3_file(const char *path, int *rate_out, int *stereo_out, char *errbuf,
    size_t errsz)
{
	FILE *fp;
	hip_t hip;
	unsigned char buf[4096];
	short dl[1152], dr[1152];
	mp3data_struct md;
	size_t nr;
	int enc_delay = -1, enc_padding = -1;
	uint64_t frames = 0;

	fp = fopen(path, "rb");
	if (fp == NULL) {
		snprintf(errbuf, errsz, "castty: cannot open %s: %s", path,
		    strerror(errno));
		return -1;
	}
	hip = hip_decode_init();
	if (hip == NULL) {
		snprintf(errbuf, errsz, "castty: hip_decode_init failed");
		fclose(fp);
		return -1;
	}

	memset(&md, 0, sizeof md);
	while ((nr = fread(buf, 1, sizeof buf, fp)) > 0) {
		const unsigned char *in = buf;
		size_t inlen = nr;

		for (;;) {
			int n = hip_decode1_headersB(hip, (unsigned char *)in,
			    inlen, dl, dr, &md, &enc_delay, &enc_padding);

			in = buf;
			inlen = 0;
			if (n < 0) {
				snprintf(errbuf, errsz,
				    "castty: %s is not a valid MP3 file", path);
				hip_decode_exit(hip);
				fclose(fp);
				return -1;
			}
			if (n == 0) {
				break;
			}
			frames += (uint64_t)n;
		}
	}

	for (;;) {
		int n = hip_decode1_headersB(hip, buf, 0, dl, dr, &md,
		    &enc_delay, &enc_padding);

		if (n <= 0) {
			break;
		}
		frames += (uint64_t)n;
	}

	hip_decode_exit(hip);
	fclose(fp);

	if (!md.header_parsed || md.samplerate <= 0 || md.stereo <= 0 ||
	    frames == 0) {
		snprintf(errbuf, errsz, "castty: %s contains no valid MP3 frames",
		    path);
		return -1;
	}

	if (rate_out) {
		*rate_out = md.samplerate;
	}
	if (stereo_out) {
		*stereo_out = (md.stereo >= 2);
	}
	return 0;
}

struct append_splice {
	int src_rate;
	int dst_rate;
	int fade_dst;
	int trim_tail_dst;
	int hold_target;
	int hold_cap;
	int hold_len;
	float *hold;
	int have_prev;
	float prev_l;
	float prev_r;
	double phase;
	double step;
	uint64_t pre_frames;
};

static void
open_append_temp_writer(void)
{
	int tfd;
	size_t tlen;

	tlen = strlen(ctx.outpath) + sizeof(".castty.XXXXXX");
	ctx.tmp_path = malloc(tlen);
	if (ctx.tmp_path == NULL) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	snprintf(ctx.tmp_path, tlen, "%s.castty.XXXXXX", ctx.outpath);
	tfd = mkstemp(ctx.tmp_path);
	if (tfd < 0) {
		perror("mkstemp");
		exit(EXIT_FAILURE);
	}
	ctx.fout = fdopen(tfd, "w+b");
	if (ctx.fout == NULL) {
		perror("fdopen");
		close(tfd);
		unlink(ctx.tmp_path);
		exit(EXIT_FAILURE);
	}

	ctx.aw_rate = ctx.stream->sample_rate;
	ctx.aw = create_audio_writer(ctx.fout, ctx.afmt, ctx.aw_rate,
	    ctx.mono && !ctx.existing_stereo);
	if (ctx.aw == NULL) {
		fprintf(stderr, "Couldn't create %s writer\n",
		    audio_format_name(ctx.afmt));
		exit(EXIT_FAILURE);
	}
}

static void
append_splice_init(struct append_splice *sp, int src_rate, int dst_rate,
    int trim_src)
{
	memset(sp, 0, sizeof *sp);
	sp->src_rate = src_rate;
	sp->dst_rate = dst_rate;
	sp->fade_dst = dst_rate / 100;
	sp->step = (double)src_rate / (double)dst_rate;
	sp->trim_tail_dst = (trim_src > 0) ?
	    (int)llround((double)trim_src * dst_rate / src_rate) : 0;
	sp->hold_target = sp->trim_tail_dst + sp->fade_dst;
	sp->hold_cap = sp->hold_target + 4096;
	sp->hold = calloc((size_t)sp->hold_cap * 2, sizeof *sp->hold);
	if (sp->hold == NULL) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
}

static void
append_splice_push_dst(struct append_splice *sp, float sl, float sr)
{
	sp->hold[(size_t)sp->hold_len * 2] = sl;
	sp->hold[(size_t)sp->hold_len * 2 + 1] = sr;
	sp->hold_len++;

	if (sp->hold_len >= sp->hold_cap) {
		int flush_n = sp->hold_len - sp->hold_target;

		audio_writer_write(ctx.aw, SoundIoFormatFloat32NE,
		    (char *)sp->hold, flush_n * 2 * (int)sizeof(float),
		    2 * (int)sizeof(float));
		sp->pre_frames += (uint64_t)flush_n;
		memmove(sp->hold, sp->hold + (size_t)flush_n * 2,
		    (size_t)sp->hold_target * 2 * sizeof *sp->hold);
		sp->hold_len = sp->hold_target;
	}
}

static void
append_splice_push(struct append_splice *sp, float sl, float sr)
{
	if (sp->src_rate == sp->dst_rate) {
		append_splice_push_dst(sp, sl, sr);
		return;
	}
	if (!sp->have_prev) {
		sp->prev_l = sl;
		sp->prev_r = sr;
		sp->have_prev = 1;
		sp->phase = 0.0;
		return;
	}
	while (sp->phase < 1.0) {
		double frac = sp->phase;

		append_splice_push_dst(sp,
		    (float)(sp->prev_l + frac * (sl - sp->prev_l)),
		    (float)(sp->prev_r + frac * (sr - sp->prev_r)));
		sp->phase += sp->step;
	}
	sp->phase -= 1.0;
	sp->prev_l = sl;
	sp->prev_r = sr;
}

static uint64_t
append_splice_finish(struct append_splice *sp)
{
	if (sp->hold == NULL) {
		return pad_audio_to_target(SoundIoFormatFloat32NE,
		    2 * (int)sizeof(float), sp->dst_rate, 0);
	}
	if (sp->src_rate != sp->dst_rate && sp->have_prev) {
		append_splice_push_dst(sp, sp->prev_l, sp->prev_r);
	}
	if (sp->hold_len > 0) {
		int trim = MIN(sp->hold_len, sp->trim_tail_dst);
		int rem = sp->hold_len - trim;
		int fade_n = MIN(rem, sp->fade_dst);
		int unfaded = rem - fade_n;

		if (unfaded > 0) {
			audio_writer_write(ctx.aw, SoundIoFormatFloat32NE,
			    (char *)sp->hold,
			    unfaded * 2 * (int)sizeof(float),
			    2 * (int)sizeof(float));
			sp->pre_frames += (uint64_t)unfaded;
		}
		if (fade_n > 0) {
			pcm_fade_frames(SoundIoFormatFloat32NE,
			    (unsigned char *)(sp->hold + (size_t)unfaded * 2),
			    fade_n, 2, 2 * (int)sizeof(float),
			    (int)sizeof(float), 0, fade_n, 1);
			audio_writer_write(ctx.aw, SoundIoFormatFloat32NE,
			    (char *)(sp->hold + (size_t)unfaded * 2),
			    fade_n * 2 * (int)sizeof(float),
			    2 * (int)sizeof(float));
			sp->pre_frames += (uint64_t)fade_n;
		}
	}

	free(sp->hold);
	sp->hold = NULL;
	return pad_audio_to_target(SoundIoFormatFloat32NE,
	    2 * (int)sizeof(float), sp->dst_rate, sp->pre_frames);
}

static uint64_t
prepare_mp3_append(void)
{
	FILE *src_fp;
	hip_t hip;
	unsigned char buf[4096];
	short dl[1152], dr[1152];
	mp3data_struct md;
	struct append_splice sp;
	int enc_delay = -1, enc_padding = -1;
	int src_rate = 0, src_ch = 0, skip_head = 0;
	int dst_rate = ctx.stream->sample_rate;
	size_t nr;

	open_append_temp_writer();
	memset(&sp, 0, sizeof sp);
	sp.dst_rate = dst_rate;

	src_fp = fopen(ctx.outpath, "rb");
	if (src_fp == NULL) {
		perror("fopen");
		exit(EXIT_FAILURE);
	}
	hip = hip_decode_init();
	if (hip == NULL) {
		fprintf(stderr, "Couldn't initialize MP3 decoder\n");
		exit(EXIT_FAILURE);
	}

	memset(&md, 0, sizeof md);
	for (;;) {
		int eof = 0;
		const unsigned char *in;
		size_t inlen;

		nr = fread(buf, 1, sizeof buf, src_fp);
		if (nr == 0) {
			eof = 1;
		}
		in = buf;
		inlen = nr;

		for (;;) {
			int n = hip_decode1_headersB(hip, (unsigned char *)in,
			    inlen, dl, dr, &md, &enc_delay, &enc_padding);
			int i;

			in = buf;
			inlen = 0;
			if (n <= 0) {
				break;
			}
			if (src_rate == 0 && md.header_parsed) {
				int trim_src = (enc_delay >= 0 &&
				    enc_padding >= 528) ? (enc_padding - 528) : 0;

				src_rate = md.samplerate;
				src_ch = md.stereo;
				skip_head = (enc_delay >= 0) ? (enc_delay + 528) : 0;
				append_splice_init(&sp, src_rate, dst_rate,
				    trim_src);
			}
			if (src_rate == 0) {
				continue;
			}

			for (i = 0; i < n; i++) {
				float sl, sr;

				if (skip_head > 0) {
					skip_head--;
					continue;
				}
				sl = (float)dl[i] / 32768.0f;
				sr = (src_ch > 1) ? ((float)dr[i] / 32768.0f) : sl;
				append_splice_push(&sp, sl, sr);
			}
		}

		if (eof) {
			break;
		}
	}

	hip_decode_exit(hip);
	fclose(src_fp);
	return append_splice_finish(&sp);
}

static uint64_t
prepare_sndfile_append(void)
{
	enum {
		SF_APPEND_CHUNK = 4096,
	};
	SF_INFO info;
	SNDFILE *sf;
	struct append_splice sp;
	float *inbuf;
	sf_count_t nread;

	open_append_temp_writer();

	memset(&info, 0, sizeof info);
	sf = sf_open(ctx.outpath, SFM_READ, &info);
	if (sf == NULL) {
		fprintf(stderr, "Couldn't open %s for append: %s\n",
		    ctx.outpath, sf_strerror(NULL));
		exit(EXIT_FAILURE);
	}

	inbuf = calloc((size_t)SF_APPEND_CHUNK * (size_t)info.channels,
	    sizeof *inbuf);
	if (inbuf == NULL) {
		perror("calloc");
		sf_close(sf);
		exit(EXIT_FAILURE);
	}

	append_splice_init(&sp, info.samplerate, ctx.stream->sample_rate, 0);
	while ((nread = sf_readf_float(sf, inbuf, SF_APPEND_CHUNK)) > 0) {
		sf_count_t i;

		for (i = 0; i < nread; i++) {
			float sl = inbuf[i * info.channels];
			float sr = (info.channels > 1) ?
			    inbuf[i * info.channels + 1] : sl;

			append_splice_push(&sp, sl, sr);
		}
	}

	free(inbuf);
	sf_close(sf);
	return append_splice_finish(&sp);
}

static uint64_t
prepare_raw_append(void)
{
	off_t raw_sz, valid_bytes;
	uint64_t frames;
	int fade_frames = ctx.stream->sample_rate / 100;

	ctx.fout = fopen(ctx.outpath, "r+b");
	if (ctx.fout == NULL) {
		perror("fopen");
		exit(EXIT_FAILURE);
	}
	if (fseeko(ctx.fout, 0, SEEK_END) != 0 ||
	    (raw_sz = ftello(ctx.fout)) < 0) {
		perror("fseeko");
		exit(EXIT_FAILURE);
	}

	valid_bytes = raw_sz - (raw_sz % ctx.frame_bytes);
	if (valid_bytes != raw_sz) {
		if (ftruncate(fileno(ctx.fout), valid_bytes) != 0) {
			perror("ftruncate");
			exit(EXIT_FAILURE);
		}
	}

	frames = (uint64_t)(valid_bytes / ctx.frame_bytes);
	if (frames > 0 && fade_frames > 0) {
		int tail_n = (frames < (uint64_t)fade_frames) ?
		    (int)frames : fade_frames;
		off_t tail_off = (off_t)(frames - (uint64_t)tail_n) *
		    ctx.frame_bytes;
		size_t tail_bytes = (size_t)tail_n * ctx.frame_bytes;
		unsigned char *tbuf = malloc(tail_bytes);

		if (tbuf == NULL) {
			perror("malloc");
			exit(EXIT_FAILURE);
		}
		if (fseeko(ctx.fout, tail_off, SEEK_SET) != 0 ||
		    fread(tbuf, 1, tail_bytes, ctx.fout) != tail_bytes) {
			perror("fread");
			free(tbuf);
			exit(EXIT_FAILURE);
		}
		pcm_fade_frames(ctx.format, tbuf, tail_n, 2, ctx.frame_bytes,
		    ctx.bytes_per_sample, 0, tail_n, 1);
		if (fseeko(ctx.fout, tail_off, SEEK_SET) != 0 ||
		    fwrite(tbuf, 1, tail_bytes, ctx.fout) != tail_bytes) {
			perror("fwrite");
			free(tbuf);
			exit(EXIT_FAILURE);
		}
		free(tbuf);
	}

	if (fseeko(ctx.fout, valid_bytes, SEEK_SET) != 0) {
		perror("fseeko");
		exit(EXIT_FAILURE);
	}

	ctx.aw_rate = ctx.stream->sample_rate;
	ctx.aw = audio_writer_raw(ctx.fout);
	if (ctx.aw == NULL) {
		fprintf(stderr, "Couldn't create raw writer\n");
		exit(EXIT_FAILURE);
	}

	return pad_audio_to_target(ctx.format, ctx.frame_bytes,
	    ctx.stream->sample_rate, frames);
}

static void *
writer(void *priv)
{
	(void)priv;

	for (;;) {
		int stop_now = atomic_load_explicit(&stopping, memory_order_acquire);
		int err = atomic_exchange_explicit(&read_err, 0, memory_order_relaxed);
		int fill_bytes = soundio_ring_buffer_fill_count(ctx.rb);

		if (err != 0) {
			fprintf(stderr, "audio read error: %s\n", soundio_strerror(err));
		}
		if (atomic_exchange_explicit(&overflowed, 0, memory_order_relaxed) != 0) {
			fprintf(stderr, "audio ring buffer overflow\n");
		}

		fill_bytes -= fill_bytes % ctx.frame_bytes;
		if (fill_bytes > 0) {
			char *read_buf = soundio_ring_buffer_read_ptr(ctx.rb);

			if (atomic_load_explicit(&recording, memory_order_acquire)) {
				if (ctx.fade_in_pos < ctx.fade_in_total) {
					int nframes = fill_bytes / ctx.frame_bytes;
					int fade_n = MIN(nframes,
					    ctx.fade_in_total - ctx.fade_in_pos);

					pcm_fade_frames(ctx.format,
					    (unsigned char *)read_buf, fade_n, 2,
					    ctx.frame_bytes, ctx.bytes_per_sample,
					    ctx.fade_in_pos, ctx.fade_in_total, 0);
					ctx.fade_in_pos += fade_n;
				}
				audio_writer_write(ctx.aw, ctx.format, read_buf,
				    fill_bytes, ctx.frame_bytes);
			}
			soundio_ring_buffer_advance_read_ptr(ctx.rb, fill_bytes);
			continue;
		}

		if (stop_now) {
			break;
		}

		usleep(1000);
	}

	fflush(ctx.fout);

	return NULL;
}

double
audio_clock_ms(void)
{
	int rate = atomic_load_explicit(&ctx.sample_rate, memory_order_acquire);
	uint_fast64_t frames = atomic_load_explicit(&ctx.clock_frames,
	    memory_order_acquire);

	if (rate <= 0) {
		return ctx.base_clock_ms;
	}
	return ctx.base_clock_ms + ((double)frames * 1000.0) / (double)rate;
}

static void
audio_record(struct SoundIoInStream *stream, int min_frames, int max_frames)
{
	struct SoundIoChannelArea *areas;
	int err, nfree;
	int frame_bytes = ctx.frame_bytes;
	int sample_bytes = ctx.bytes_per_sample;
	int is_recording = atomic_load_explicit(&recording, memory_order_acquire);
	int is_muted = atomic_load_explicit(&muted, memory_order_relaxed);
	char *buf;

	buf = soundio_ring_buffer_write_ptr(ctx.rb);
	nfree = soundio_ring_buffer_free_count(ctx.rb) / frame_bytes;

	if (!is_recording || nfree < min_frames) {
		int drop_rem = max_frames;

		if (is_recording && nfree < min_frames) {
			atomic_store_explicit(&overflowed, 1, memory_order_relaxed);
		}
		while (drop_rem > 0 && drop_rem >= min_frames) {
			int nframe = drop_rem;

			if ((err = soundio_instream_begin_read(stream, &areas, &nframe)) != 0) {
				atomic_store_explicit(&read_err, err, memory_order_relaxed);
				return;
			}
			if (!nframe) {
				break;
			}
			if ((err = soundio_instream_end_read(stream)) != 0) {
				atomic_store_explicit(&read_err, err, memory_order_relaxed);
				return;
			}
			drop_rem -= nframe;
		}
		return;
	}

	int to_write = MIN(nfree, max_frames);
	int remaining = to_write;
	int written_frames = 0;

	while (remaining > 0 && remaining >= min_frames) {
		int nframe = remaining;

		if ((err = soundio_instream_begin_read(stream, &areas, &nframe)) != 0) {
			atomic_store_explicit(&read_err, err, memory_order_relaxed);
			break;
		}
		if (!nframe) {
			break;
		}

		if (!areas || is_muted) {
			memset(buf, 0, (size_t)nframe * frame_bytes);
			buf += (size_t)nframe * frame_bytes;
		} else {
			for (int frame = 0; frame < nframe; frame++) {
				memcpy(buf, areas[0].ptr, sample_bytes);
				buf += sample_bytes;
				if (ctx.mono) {
					memcpy(buf, areas[0].ptr, sample_bytes);
				} else {
					memcpy(buf, areas[1].ptr, sample_bytes);
					areas[1].ptr += areas[1].step;
				}
				buf += sample_bytes;
				areas[0].ptr += areas[0].step;
			}
		}

		written_frames += nframe;
		remaining -= nframe;

		if ((err = soundio_instream_end_read(stream)) != 0) {
			atomic_store_explicit(&read_err, err, memory_order_relaxed);
			break;
		}
	}

	if (written_frames > 0) {
		soundio_ring_buffer_advance_write_ptr(ctx.rb,
		    written_frames * frame_bytes);
		atomic_fetch_add_explicit(&ctx.clock_frames,
		    (uint_fast64_t)written_frames, memory_order_release);
	}
}

void
audio_toggle_mp3(void)
{

	requested_fmt = (requested_fmt == AUDIO_FMT_MP3) ?
	    AUDIO_FMT_RAW : AUDIO_FMT_MP3;
}

void
audio_toggle_mute(void)
{
	int cur;

	if (!ctx.active) {
		return;
	}

	cur = atomic_load_explicit(&muted, memory_order_relaxed);
	atomic_store_explicit(&muted, !cur, memory_order_relaxed);
}

void
audio_start(void)
{
	static const struct SoundIoChannelLayout *stereo, *mono;
	int err;

	if (!ctx.active || ctx.started) {
		return;
	}

	if (stereo == NULL) {
		stereo = soundio_channel_layout_get_builtin(SoundIoChannelLayoutIdStereo);
		mono = soundio_channel_layout_get_builtin(SoundIoChannelLayoutIdMono);
	}

	ctx.io = soundio_create();
	if (ctx.io == NULL) {
		fprintf(stderr, "Couldn't initialize audio\n");
		exit(EXIT_FAILURE);
	}

	if ((err = soundio_connect(ctx.io))) {
		fprintf(stderr, "Couldn't connect to audio backend: %s\n",
		    soundio_strerror(err));
		audio_teardown_backend();
		exit(EXIT_FAILURE);
	}
	ctx.connected = 1;

	soundio_flush_events(ctx.io);

	int ndev = soundio_input_device_count(ctx.io);
	if (ndev == 0) {
		fprintf(stderr, "No input devices available.\n");
		audio_teardown_backend();
		exit(EXIT_FAILURE);
	}

	for (int i = 0; i < ndev; i++) {
		ctx.dev = soundio_get_input_device(ctx.io, i);
		if (!strcmp(ctx.dev->id, ctx.devid) && ctx.dev->is_raw == ctx.use_raw) {
			break;
		}

		soundio_device_unref(ctx.dev);
		ctx.dev = NULL;
	}

	if (ctx.dev == NULL) {
		fprintf(stderr, "Couldn't find requested device\n");
		audio_teardown_backend();
		exit(EXIT_FAILURE);
	}

	if (ctx.dev->probe_error) {
		fprintf(stderr, "Device error while probing: %s\n",
		    soundio_strerror(ctx.dev->probe_error));
		audio_teardown_backend();
		exit(EXIT_FAILURE);
	}

	soundio_device_sort_channel_layouts(ctx.dev);

	ctx.stream = soundio_instream_create(ctx.dev);
	if (ctx.stream == NULL) {
		fprintf(stderr, "Couldn't create stream\n");
		audio_teardown_backend();
		exit(EXIT_FAILURE);
	}

	if (soundio_device_supports_layout(ctx.dev, stereo)) {
		ctx.stream->layout = *stereo;
		ctx.mono = 0;
	} else if (soundio_device_supports_layout(ctx.dev, mono)) {
		ctx.stream->layout = *mono;
		ctx.mono = 1;
	} else if (ctx.dev->layout_count > 0 &&
	    ctx.dev->layouts[0].channel_count >= 1) {
		ctx.stream->layout = ctx.dev->layouts[0];
		ctx.mono = (ctx.stream->layout.channel_count == 1);
	} else {
		fprintf(stderr, "Sound device doesn't support stereo"
		    " or mono.\n");
		audio_teardown_backend();
		exit(EXIT_FAILURE);
	}

	for (unsigned i = 0; i < sizeof formats / sizeof formats[0]; i++) {
		if (formats[i] == SoundIoFormatInvalid) {
			fprintf(stderr, "Input device supports no usable formats\n");
			audio_teardown_backend();
			exit(EXIT_FAILURE);
		}

		if (soundio_device_supports_format(ctx.dev, formats[i])) {
			ctx.stream->format = formats[i];
			break;
		}
	}

	int target_rate = ctx.aw_rate > 0 ? ctx.aw_rate : ctx.pref_rate;
	const int *cand_rates = (ctx.afmt == AUDIO_FMT_OPUS) ? opus_rates : rates;

	if (target_rate > 0 &&
	    soundio_device_supports_sample_rate(ctx.dev, target_rate)) {
		ctx.stream->sample_rate = target_rate;
	} else {
		for (unsigned i = 0;; i++) {
			if (cand_rates[i] == 0) {
				if (ctx.dev->sample_rate_current > 0 &&
				    soundio_device_supports_sample_rate(ctx.dev,
				    ctx.dev->sample_rate_current) &&
				    (ctx.afmt != AUDIO_FMT_OPUS ||
				    is_opus_rate(ctx.dev->sample_rate_current))) {
					ctx.stream->sample_rate = ctx.dev->sample_rate_current;
					break;
				}
				fprintf(stderr, "Input device supports no usable rates\n");
				audio_teardown_backend();
				exit(EXIT_FAILURE);
			}

			if (soundio_device_supports_sample_rate(ctx.dev, cand_rates[i])) {
				ctx.stream->sample_rate = cand_rates[i];
				break;
			}
		}
	}

	ctx.stream->read_callback = audio_record;
	ctx.stream->overflow_callback = NULL;
	ctx.stream->userdata = NULL;

	if ((err = soundio_instream_open(ctx.stream))) {
		fprintf(stderr, "Couldn't open stream: %s\n",
		    soundio_strerror(err));
		audio_teardown_backend();
		exit(EXIT_FAILURE);
	}

	ctx.bytes_per_sample = ctx.stream->bytes_per_sample;
	ctx.frame_bytes = 2 * ctx.bytes_per_sample;
	ctx.format = ctx.stream->format;
	atomic_store_explicit(&ctx.sample_rate, ctx.stream->sample_rate,
	    memory_order_release);
	atomic_store_explicit(&ctx.clock_frames, 0, memory_order_release);
	atomic_store_explicit(&overflowed, 0, memory_order_relaxed);
	atomic_store_explicit(&read_err, 0, memory_order_relaxed);

	ctx.rb = soundio_ring_buffer_create(ctx.io,
	    BUF_TIME_S * ctx.stream->sample_rate * ctx.frame_bytes);
	if (ctx.rb == NULL) {
		fprintf(stderr, "\rCouldn't allocate ring buffer for audio\n");
		audio_teardown_backend();
		exit(EXIT_FAILURE);
	}

	if (!ctx.prepared) {
		struct stat st;
		uint64_t pre_frames = 0;

		if (ctx.append && stat(ctx.outpath, &st) == 0 && st.st_size > 0) {
			if (ctx.afmt == AUDIO_FMT_MP3) {
				pre_frames = prepare_mp3_append();
			} else if (audio_sndfile_format(ctx.afmt) != 0) {
				pre_frames = prepare_sndfile_append();
			} else {
				pre_frames = prepare_raw_append();
			}
			ctx.base_clock_ms = ((double)pre_frames * 1000.0) /
			    (double)ctx.stream->sample_rate;
		} else {
			if (ctx.fout == NULL) {
				ctx.fout = xfopen(ctx.outpath, "w+b");
			}
			ctx.aw_rate = ctx.stream->sample_rate;
			ctx.aw = create_audio_writer(ctx.fout, ctx.afmt,
			    ctx.aw_rate, ctx.mono);
			if (ctx.aw == NULL) {
				fprintf(stderr,
				    "Couldn't initialize %s audio writer\n",
				    audio_format_name(ctx.afmt));
				exit(EXIT_FAILURE);
			}
			if (ctx.append && ctx.target_cast_ms > 0.0) {
				pre_frames = pad_audio_to_target(ctx.format,
				    ctx.frame_bytes, ctx.aw_rate, 0);
				if (pre_frames > 0) {
					ctx.base_clock_ms = ((double)pre_frames * 1000.0) /
					    (double)ctx.aw_rate;
				}
			}
		}
		ctx.prepared = 1;
	}

	ctx.fade_in_total = ctx.stream->sample_rate / 100;
	ctx.fade_in_pos = 0;

	atomic_store_explicit(&stopping, 0, memory_order_release);
	atomic_store_explicit(&recording, 1, memory_order_release);

	err = soundio_instream_start(ctx.stream);
	if (err) {
		fprintf(stderr, "Error recording: %s\n", soundio_strerror(err));
		atomic_store_explicit(&recording, 0, memory_order_release);
		audio_teardown_backend();
		exit(EXIT_FAILURE);
	}

	if (pthread_create(&wthread, NULL, writer, NULL) != 0) {
		perror("pthread_create");
		atomic_store_explicit(&recording, 0, memory_order_release);
		audio_teardown_backend();
		if (ctx.fout) {
			xfclose(ctx.fout);
			ctx.fout = NULL;
		}
		if (ctx.tmp_path) {
			unlink(ctx.tmp_path);
		}
		exit(EXIT_FAILURE);
	}

	ctx.started = 1;
}

int
audio_check_append(const char *outfile, char *errbuf, size_t errsz)
{
	struct stat st;
	enum audio_format afmt;

	if (outfile == NULL) {
		return 0;
	}
	afmt = resolve_audio_format(outfile);
	ctx.afmt = afmt;
	if (stat(outfile, &st) != 0) {
		if (errno == ENOENT) {
			return 0;
		}
		snprintf(errbuf, errsz, "castty: cannot access %s: %s",
		    outfile, strerror(errno));
		return -1;
	}
	if (st.st_size == 0) {
		return 0;
	}
	if (afmt == AUDIO_FMT_MP3) {
		int rate = 0, stereo = 0;

		if (probe_mp3_file(outfile, &rate, &stereo, errbuf, errsz) != 0) {
			return -1;
		}
		ctx.pref_rate = rate;
		ctx.existing_stereo = stereo;
		return 0;
	} else if (audio_sndfile_format(afmt) != 0) {
		int rate = 0, stereo = 0;

		if (probe_sndfile_file(outfile, afmt, &rate, &stereo, errbuf,
		    errsz) != 0) {
			return -1;
		}
		ctx.pref_rate = rate;
		ctx.existing_stereo = stereo;
		return 0;
	} else {
		FILE *fp = fopen(outfile, "r+b");

		if (fp == NULL) {
			snprintf(errbuf, errsz,
			    "castty: cannot open %s for append: %s",
			    outfile, strerror(errno));
			return -1;
		}
		fclose(fp);
		return 0;
	}
}

void
audio_init_append(const char *devid, const char *outfile, int use_raw,
    int append, double target_cast_ms)
{

	ctx.active = 1;
	ctx.started = 0;
	ctx.prepared = 0;
	ctx.append = append;
	ctx.afmt = resolve_audio_format(outfile);
	ctx.target_cast_ms = target_cast_ms;
	ctx.base_clock_ms = 0.0;
	ctx.aw = NULL;
	ctx.aw_rate = 0;
	ctx.fade_in_total = 0;
	ctx.fade_in_pos = 0;
	atomic_init(&ctx.clock_frames, 0);
	atomic_init(&ctx.sample_rate, 0);
	free(ctx.outpath);
	ctx.outpath = strdup(outfile);
	if (ctx.outpath == NULL) {
		perror("strdup");
		exit(EXIT_FAILURE);
	}
	free(ctx.tmp_path);
	ctx.tmp_path = NULL;
	if (!append) {
		ctx.pref_rate = 0;
		ctx.existing_stereo = 0;
		ctx.fout = xfopen(outfile, "w+b");
	} else {
		ctx.fout = NULL;
	}
	ctx.devid = devid;
	ctx.use_raw = use_raw;
}

void
audio_init(const char *devid, const char *outfile, int use_raw)
{

	audio_init_append(devid, outfile, use_raw, 0, 0.0);
}

void
audio_stop(void)
{
	int rate;
	uint_fast64_t frames;

	if (!ctx.active || !ctx.started) {
		return;
	}

	if (ctx.stream) {
		soundio_instream_destroy(ctx.stream);
		ctx.stream = NULL;
	}

	rate = atomic_load_explicit(&ctx.sample_rate, memory_order_acquire);
	frames = atomic_load_explicit(&ctx.clock_frames, memory_order_acquire);
	if (rate > 0) {
		ctx.base_clock_ms += ((double)frames * 1000.0) / (double)rate;
	}
	atomic_store_explicit(&ctx.clock_frames, 0, memory_order_release);

	atomic_store_explicit(&stopping, 1, memory_order_release);
	pthread_join(wthread, NULL);

	audio_teardown_backend();
	atomic_store_explicit(&recording, 0, memory_order_release);
	atomic_store_explicit(&stopping, 0, memory_order_release);
	ctx.started = 0;
}

void
audio_exit(void)
{
	if (ctx.started) {
		audio_stop();
	}
	ctx.active = 0;

	if (ctx.aw) {
		audio_writer_destroy(ctx.aw);
		ctx.aw = NULL;
	}

	if (ctx.fout) {
		xfclose(ctx.fout);
		ctx.fout = NULL;
	}

	if (ctx.tmp_path) {
		if (ctx.prepared) {
			if (rename(ctx.tmp_path, ctx.outpath) != 0) {
				perror("rename");
				exit(EXIT_FAILURE);
			}
		} else {
			unlink(ctx.tmp_path);
		}
		free(ctx.tmp_path);
		ctx.tmp_path = NULL;
	}

	free(ctx.outpath);
	ctx.outpath = NULL;
}

void
audio_list_inputs(void)
{
	struct SoundIo *soundio;
	int err;

	soundio = soundio_create();
	if (soundio == NULL) {
		fprintf(stderr, "Couldn't initialize audio system\n");
		exit(EXIT_FAILURE);
	}

	if ((err = soundio_connect(soundio))) {
		fprintf(stderr, "Couldn't connect to audio backend: %s",
		    soundio_strerror(err));
		exit(EXIT_FAILURE);
	}

	soundio_flush_events(soundio);

	int ndev = soundio_input_device_count(soundio);
	if (ndev == 0) {
		fprintf(stderr, "No input devices available.\n");
		soundio_disconnect(soundio);
		soundio_destroy(soundio);
		exit(EXIT_FAILURE);
	}


	printf("Available input devices:\n");

	for (int i = 0; i < ndev; i++) {
		const struct SoundIoChannelLayout *stereo, *mono;
		struct SoundIoDevice *dev;
		enum SoundIoFormat fmt;
		int rate;

		dev = soundio_get_input_device(soundio, i);
		if (dev->probe_error) {
			soundio_device_unref(dev);
			continue;
		}

		stereo = soundio_channel_layout_get_builtin(SoundIoChannelLayoutIdStereo);
		mono = soundio_channel_layout_get_builtin(SoundIoChannelLayoutIdMono);
		if (!soundio_device_supports_layout(dev, stereo) &&
		    !soundio_device_supports_layout(dev, mono)) {
			soundio_device_unref(dev);
			continue;
		}

		fmt = SoundIoFormatInvalid;
		for (unsigned j = 0; j < sizeof formats / sizeof formats[0]; j++) {
			if (soundio_device_supports_format(dev, formats[j])) {
				fmt = formats[j];
				break;
			}
		}

		if (fmt == SoundIoFormatInvalid) {
			soundio_device_unref(dev);
			continue;
		}

		rate = 0;
		for (unsigned j = 0; j < sizeof rates / sizeof rates[0]; j++) {
			if (soundio_device_supports_sample_rate(dev, rates[j])) {
				rate = rates[j];
				break;
			}
		}

		if (rate == 0) {
			soundio_device_unref(dev);
			continue;
		}
		printf("%4d: %s%s, %dHz, format: '%s'\n"
		    "      castty record -d '%s'%s -a audio.raw\n",
		    i, dev->name, dev->is_raw ? " (raw)" : "",
		    rate, soundio_format_string(fmt),
		    dev->id, dev->is_raw ? " -R" : "");
		soundio_device_unref(dev);
	}

	soundio_disconnect(soundio);
	soundio_destroy(soundio);

	exit(EXIT_SUCCESS);
}

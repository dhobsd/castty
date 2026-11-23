#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include <pthread.h>
#include <unistd.h>

#include <soundio/soundio.h>
#include <lame/lame.h>

#include "castty.h"
#include "audio/pcm.h"
#include "audio/writer.h"
#include "audio/writer-lame.h"
#include "audio/writer-raw.h"

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
static int mp3;

static pthread_t wthread;

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
	size_t chunk_bytes = (size_t)ZERO_CHUNK_FRAMES * frame_bytes;

	if (nframes == 0) {
		return;
	}
	zbuf = calloc(1, chunk_bytes);
	if (zbuf == NULL) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	while (nframes > 0) {
		uint64_t chunk = nframes > ZERO_CHUNK_FRAMES ?
		    ZERO_CHUNK_FRAMES : nframes;

		audio_writer_write(aw, fmt, (char *)zbuf,
		    (int)(chunk * frame_bytes), frame_bytes);
		nframes -= chunk;
	}
	free(zbuf);
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

static uint64_t
prepare_mp3_append(void)
{
	FILE *src_fp;
	hip_t hip;
	unsigned char buf[4096];
	short dl[1152], dr[1152];
	mp3data_struct md;
	int enc_delay = -1, enc_padding = -1;
	int src_rate = 0, src_ch = 0;
	int dst_rate = ctx.stream->sample_rate;
	int skip_head = 0, trim_tail_dst = 0;
	int fade_dst = dst_rate / 100;
	int hold_target = 0, hold_cap = 0, hold_len = 0;
	float *hold = NULL;
	int have_prev = 0;
	float prev_l = 0.0f, prev_r = 0.0f;
	double phase = 0.0, step = 1.0;
	uint64_t pre_frames = 0;
	size_t nr;
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

	ctx.aw_rate = dst_rate;
	ctx.aw = audio_writer_lame(ctx.fout, dst_rate, 2, BUF_TIME_S,
	    ctx.mono && !ctx.existing_stereo);
	if (ctx.aw == NULL) {
		fprintf(stderr, "Couldn't create MP3 writer\n");
		exit(EXIT_FAILURE);
	}

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
				int trim_src;

				src_rate = md.samplerate;
				src_ch = md.stereo;
				step = (double)src_rate / (double)dst_rate;
				skip_head = (enc_delay >= 0) ? (enc_delay + 528) : 0;
				trim_src = (enc_delay >= 0 && enc_padding >= 528) ?
				    (enc_padding - 528) : 0;
				trim_tail_dst = (int)llround((double)trim_src *
				    dst_rate / src_rate);
				hold_target = trim_tail_dst + fade_dst;
				hold_cap = hold_target + 4096;
				hold = calloc((size_t)hold_cap * 2, sizeof *hold);
				if (hold == NULL) {
					perror("calloc");
					exit(EXIT_FAILURE);
				}
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

				if (src_rate == dst_rate) {
					hold[(size_t)hold_len * 2] = sl;
					hold[(size_t)hold_len * 2 + 1] = sr;
					hold_len++;
				} else if (!have_prev) {
					prev_l = sl;
					prev_r = sr;
					have_prev = 1;
					phase = 0.0;
				} else {
					while (phase < 1.0) {
						double frac = phase;

						hold[(size_t)hold_len * 2] =
						    (float)(prev_l + frac * (sl - prev_l));
						hold[(size_t)hold_len * 2 + 1] =
						    (float)(prev_r + frac * (sr - prev_r));
						hold_len++;
						if (hold_len >= hold_cap) {
							int flush_n = hold_len - hold_target;

							audio_writer_write(ctx.aw,
							    SoundIoFormatFloat32NE,
							    (char *)hold,
							    flush_n * 2 * (int)sizeof(float),
							    2 * (int)sizeof(float));
							pre_frames += (uint64_t)flush_n;
							memmove(hold,
							    hold + (size_t)flush_n * 2,
							    (size_t)hold_target * 2 * sizeof *hold);
							hold_len = hold_target;
						}
						phase += step;
					}
					phase -= 1.0;
					prev_l = sl;
					prev_r = sr;
				}

				if (hold_len >= hold_cap) {
					int flush_n = hold_len - hold_target;

					audio_writer_write(ctx.aw,
					    SoundIoFormatFloat32NE,
					    (char *)hold,
					    flush_n * 2 * (int)sizeof(float),
					    2 * (int)sizeof(float));
					pre_frames += (uint64_t)flush_n;
					memmove(hold,
					    hold + (size_t)flush_n * 2,
					    (size_t)hold_target * 2 * sizeof *hold);
					hold_len = hold_target;
				}
			}
		}

		if (eof) {
			break;
		}
	}

	if (src_rate != dst_rate && have_prev && hold != NULL && hold_len < hold_cap) {
		hold[(size_t)hold_len * 2] = prev_l;
		hold[(size_t)hold_len * 2 + 1] = prev_r;
		hold_len++;
	}

	if (hold != NULL && hold_len > 0) {
		int trim = MIN(hold_len, trim_tail_dst);
		int rem = hold_len - trim;
		int fade_n = MIN(rem, fade_dst);
		int unfaded = rem - fade_n;

		if (unfaded > 0) {
			audio_writer_write(ctx.aw, SoundIoFormatFloat32NE,
			    (char *)hold,
			    unfaded * 2 * (int)sizeof(float),
			    2 * (int)sizeof(float));
			pre_frames += (uint64_t)unfaded;
		}
		if (fade_n > 0) {
			pcm_fade_frames(SoundIoFormatFloat32NE,
			    (unsigned char *)(hold + (size_t)unfaded * 2),
			    fade_n, 2, 2 * (int)sizeof(float),
			    (int)sizeof(float), 0, fade_n, 1);
			audio_writer_write(ctx.aw, SoundIoFormatFloat32NE,
			    (char *)(hold + (size_t)unfaded * 2),
			    fade_n * 2 * (int)sizeof(float),
			    2 * (int)sizeof(float));
			pre_frames += (uint64_t)fade_n;
		}
	}

	free(hold);
	hip_decode_exit(hip);
	fclose(src_fp);

	if (ctx.target_cast_ms > 0.0) {
		double cur_ms = ((double)pre_frames * 1000.0) / (double)dst_rate;

		if (cur_ms < ctx.target_cast_ms) {
			uint64_t want = (uint64_t)llround(ctx.target_cast_ms *
			    (double)dst_rate / 1000.0);

			if (want > pre_frames) {
				write_zero_frames(ctx.aw, SoundIoFormatFloat32NE,
				    2 * (int)sizeof(float), want - pre_frames);
				pre_frames = want;
			}
		}
	}

	return pre_frames;
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

	if (ctx.target_cast_ms > 0.0) {
		double cur_ms = ((double)frames * 1000.0) /
		    (double)ctx.stream->sample_rate;

		if (cur_ms < ctx.target_cast_ms) {
			uint64_t want = (uint64_t)llround(ctx.target_cast_ms *
			    (double)ctx.stream->sample_rate / 1000.0);

			if (want > frames) {
				write_zero_frames(ctx.aw, ctx.format,
				    ctx.frame_bytes, want - frames);
				frames = want;
			}
		}
	}

	return frames;
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

	mp3 = !mp3;
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

	if (target_rate > 0 &&
	    soundio_device_supports_sample_rate(ctx.dev, target_rate)) {
		ctx.stream->sample_rate = target_rate;
	} else {
		for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) {
			if (rates[i] == 0) {
				if (ctx.dev->sample_rate_current > 0 &&
				    soundio_device_supports_sample_rate(ctx.dev,
				    ctx.dev->sample_rate_current)) {
					ctx.stream->sample_rate = ctx.dev->sample_rate_current;
					break;
				}
				fprintf(stderr, "Input device supports no usable rates\n");
				audio_teardown_backend();
				exit(EXIT_FAILURE);
			}

			if (soundio_device_supports_sample_rate(ctx.dev, rates[i])) {
				ctx.stream->sample_rate = rates[i];
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
			if (mp3) {
				pre_frames = prepare_mp3_append();
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
			ctx.aw = mp3 ? audio_writer_lame(ctx.fout, ctx.aw_rate, 2,
			    BUF_TIME_S, ctx.mono) : audio_writer_raw(ctx.fout);
			if (ctx.aw == NULL) {
				fprintf(stderr,
				    "castty was not compiled with support for the selected format\n");
				exit(EXIT_FAILURE);
			}
			if (ctx.append && ctx.target_cast_ms > 0.0) {
				pre_frames = (uint64_t)llround(ctx.target_cast_ms *
				    (double)ctx.aw_rate / 1000.0);
				if (pre_frames > 0) {
					write_zero_frames(ctx.aw, ctx.format,
					    ctx.frame_bytes, pre_frames);
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

	if (outfile == NULL) {
		return 0;
	}
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
	if (mp3) {
		int rate = 0, stereo = 0;

		if (probe_mp3_file(outfile, &rate, &stereo, errbuf, errsz) != 0) {
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

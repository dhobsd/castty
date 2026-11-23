#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <pthread.h>
#include <unistd.h>

#include <soundio/soundio.h>

#include "castty.h"
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
	atomic_uint_fast64_t clock_frames;
	double base_clock_ms;
	atomic_int sample_rate;
	enum SoundIoFormat format;
	int bytes_per_sample;
	int frame_bytes;
	FILE *fout;
	int active;
	int started;
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

static void *
writer(void *priv)
{
	struct audio_writer *aw;
	int sample_rate;

	(void)priv;

	sample_rate = atomic_load_explicit(&ctx.sample_rate, memory_order_acquire);
	(void)sample_rate;
	if (mp3) {
		aw = audio_writer_lame(ctx.fout, sample_rate, 2,
		    BUF_TIME_S, ctx.mono);
	} else {
		aw = audio_writer_raw(ctx.fout);
	}

	if (aw == NULL) {
		fprintf(stderr, "castty was not compiled with support for the selected format\n");
		exit(EXIT_FAILURE);
	}

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
				audio_writer_write(aw, ctx.format, read_buf,
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

	audio_writer_destroy(aw);
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
		xfclose(ctx.fout);
		ctx.fout = NULL;
		exit(EXIT_FAILURE);
	}

	ctx.started = 1;
}

void
audio_init(const char *devid, const char *outfile, int use_raw)
{

	ctx.active = 1;
	ctx.started = 0;
	ctx.base_clock_ms = 0.0;
	atomic_init(&ctx.clock_frames, 0);
	atomic_init(&ctx.sample_rate, 0);
	ctx.fout = xfopen(outfile, "wb");
	ctx.devid = devid;
	ctx.use_raw = use_raw;
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

	if (ctx.fout) {
		xfclose(ctx.fout);
		ctx.fout = NULL;
	}
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

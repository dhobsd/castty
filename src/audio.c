#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <soundio/soundio.h>

#include "castty.h"
#include "audio/writer.h"
#include "audio/writer-lame.h"
#include "audio/writer-raw.h"

static enum SoundIoFormat formats[] = {
	SoundIoFormatFloat32LE,
	SoundIoFormatFloat32BE,
	SoundIoFormatU32BE,
	SoundIoFormatU32LE,
	SoundIoFormatS32BE,
	SoundIoFormatS32LE,
	SoundIoFormatS24BE,
	SoundIoFormatS24LE,
	SoundIoFormatU24BE,
	SoundIoFormatU24LE,
	SoundIoFormatS16BE,
	SoundIoFormatS16LE,
	SoundIoFormatU16BE,
	SoundIoFormatU16LE,
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
	double clock;
	FILE *fout;
	int active;
	int mono;

	struct SoundIoInStream *stream;
	struct SoundIoRingBuffer *rb;
	struct SoundIoDevice *dev;
	struct SoundIo *io;
} ctx;

static volatile int recording;
static int post = 2;
static int muted;
static int mp3;

pthread_t wthread, rthread;

static void *
writer(void *priv)
{
	struct audio_writer *aw;

	(void)priv;

	stack_t ss;
	memset(&ss, 0, sizeof(ss));
	ss.ss_size = 4 * SIGSTKSZ;
	ss.ss_sp = calloc(1, ss.ss_size);
	if (ss.ss_sp == NULL) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	sigaltstack(&ss, 0);

	__sync_fetch_and_sub(&post, 1);
	while (post > 0 || recording == 0) {
		usleep(10);
	}

	if (mp3) {
		aw = audio_writer_lame(ctx.fout, ctx.stream->sample_rate,
		    ctx.stream->layout.channel_count, BUF_TIME_S, ctx.mono);
	} else {
		aw = audio_writer_raw(ctx.fout);
	}

	if (aw == NULL) {
		fprintf(stderr, "castty was not compiled with support for the selected format");
		exit(EXIT_FAILURE);
	}

	while (1) {
		int fill_bytes = soundio_ring_buffer_fill_count(ctx.rb);
		char *read_buf = soundio_ring_buffer_read_ptr(ctx.rb);

		if (recording) {
			audio_writer_write(aw, ctx.stream->format, read_buf, fill_bytes,
			    ctx.stream->bytes_per_frame);
		}
		soundio_ring_buffer_advance_read_ptr(ctx.rb, fill_bytes);

		usleep(10);
		if (post > 0) {
			break;
		}
	}

	audio_writer_destroy(aw);

	return NULL;
}

double
audio_clock_ms(void)
{

	return (ctx.clock * 1000.) / (double)ctx.stream->sample_rate;
}

static void
audio_record(struct SoundIoInStream *stream, int min_frames, int max_frames)
{
	struct SoundIoChannelArea *areas;
	int err, nfree;
	char *buf;

	buf = soundio_ring_buffer_write_ptr(ctx.rb);
	nfree = soundio_ring_buffer_free_count(ctx.rb) / stream->bytes_per_frame;

	if (nfree < min_frames) {
		fprintf(stderr, "ring buffer overflow\n");
		exit(EXIT_FAILURE);
	}

	int to_write = MIN(nfree, max_frames);
	int remaining = to_write;

	if (!recording) {
		soundio_ring_buffer_advance_write_ptr(ctx.rb,
		    to_write * stream->bytes_per_frame);
		return;
	}

	while (remaining > 0) {
		int nframe = remaining;

		if ((err = soundio_instream_begin_read(stream, &areas, &nframe))) {
			fprintf(stderr, "begin read error: %s", soundio_strerror(err));
			exit(EXIT_FAILURE);
		}

		if (!nframe)
			break;

		if (!areas || muted) {
			memset(buf, 0, nframe * stream->bytes_per_frame);
			ctx.clock += nframe;
		} else {
			int off = nframe * stream->bytes_per_sample;
			for (int frame = 0; frame < nframe; frame++) {
				int ch = 0;
				memcpy(buf, areas[ch].ptr, stream->bytes_per_sample);

				if (!ctx.mono) {
					areas[ch].ptr += areas[ch].step;
					ch++;
				}

				/* Don't interleave when outputting MP3 */
				if (!mp3) {
					buf += stream->bytes_per_sample;
					memcpy(buf, areas[ch].ptr, stream->bytes_per_sample);
					buf += stream->bytes_per_sample;
				} else {
					memcpy(buf + off, areas[ch].ptr, stream->bytes_per_sample);
					buf += stream->bytes_per_sample;
				}

				areas[ch].ptr += areas[ch].step;
				ctx.clock++;
			}
		}

		if ((err = soundio_instream_end_read(stream))) {
			fprintf(stderr, "end read error: %s", soundio_strerror(err));
			exit(EXIT_FAILURE);
		}

		remaining -= nframe;
	}

	soundio_ring_buffer_advance_write_ptr(ctx.rb, to_write * stream->bytes_per_frame);
}

static void *
reader(void *priv)
{

	(void)priv;

	stack_t ss;
	memset(&ss, 0, sizeof(ss));
	ss.ss_size = 4 * SIGSTKSZ;
	ss.ss_sp = calloc(1, ss.ss_size);
	if (ss.ss_sp == NULL) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	sigaltstack(&ss, 0);

	__sync_fetch_and_sub(&post, 1);
	while (post > 0 || recording == 0) {
		usleep(10);
	}

	while (1) {
		soundio_flush_events(ctx.io);
		usleep(100);

		if (post > 0) {
			usleep(1000);
			break;
		}
	}

	return NULL;
}

void
audio_toggle_mp3(void)
{

	mp3 = !mp3;
}

void
audio_toggle_mute(void)
{

	if (!ctx.active) {
		return;
	}

	muted = !muted;
}

void
audio_start(void)
{
	static const struct SoundIoChannelLayout *stereo, *mono;
	int err;

	if (!ctx.active) {
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
		fprintf(stderr, "Couldn't connect to audio backend: %s",
		    soundio_strerror(err));
		exit(EXIT_FAILURE);
	}

	soundio_flush_events(ctx.io);

	int ndev = soundio_input_device_count(ctx.io);
	if (ndev == 0) {
		fprintf(stderr, "No input devices available.\n");
		soundio_disconnect(ctx.io);
		soundio_destroy(ctx.io);
		exit(EXIT_FAILURE);
	}

	for (int i = 0; i < ndev; i++) {
		ctx.dev = soundio_get_input_device(ctx.io, i);
		if (!strcmp(ctx.dev->id, ctx.devid)) {
			break;
		}

		soundio_device_unref(ctx.dev);
		ctx.dev = NULL;
	}

	if (ctx.dev == NULL) {
		fprintf(stderr, "Couldn't find requested device\n");
		soundio_device_unref(ctx.dev);
		soundio_disconnect(ctx.io);
		soundio_destroy(ctx.io);
		exit(EXIT_FAILURE);
	}

	if (ctx.dev->probe_error) {
		fprintf(stderr, "Device error while probing: %s\n",
		    soundio_strerror(ctx.dev->probe_error));
		soundio_device_unref(ctx.dev);
		soundio_disconnect(ctx.io);
		soundio_destroy(ctx.io);
		exit(EXIT_FAILURE);
	}

	soundio_device_sort_channel_layouts(ctx.dev);

	ctx.stream = soundio_instream_create(ctx.dev);
	if (ctx.stream == NULL) {
		fprintf(stderr, "Couldn't create stream\n");
		soundio_device_unref(ctx.dev);
		soundio_disconnect(ctx.io);
		soundio_destroy(ctx.io);
		exit(EXIT_FAILURE);
	}

	if (soundio_device_supports_layout(ctx.dev, stereo)) {
		ctx.stream->layout = *stereo;
		ctx.mono = 0;
	} else if (soundio_device_supports_layout(ctx.dev, mono)) {
		ctx.stream->layout = *mono;
		ctx.mono = 1;
	} else {
		fprintf(stderr, "Sound device doesn't support stereo"
		    " or mono.\n");
		soundio_device_unref(ctx.dev);
		soundio_disconnect(ctx.io);
		soundio_destroy(ctx.io);
		exit(EXIT_FAILURE);
	}

	for (unsigned i = 0; i < sizeof formats / sizeof formats[0]; i++) {
		if (formats[i] == SoundIoFormatInvalid) {
			fprintf(stderr, "Input device supports no usable formats\n");
			soundio_instream_destroy(ctx.stream);
			soundio_disconnect(ctx.io);
			soundio_destroy(ctx.io);
			exit(EXIT_FAILURE);
		}

		if (soundio_device_supports_format(ctx.dev, formats[i])) {
			ctx.stream->format = formats[i];
			break;
		}
	}

	for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) {
		if (rates[i] == 0) {
			fprintf(stderr, "Input device supports no usable rates\n");
			soundio_device_unref(ctx.dev);
			soundio_instream_destroy(ctx.stream);
			soundio_disconnect(ctx.io);
			soundio_destroy(ctx.io);
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
		soundio_device_unref(ctx.dev);
		soundio_instream_destroy(ctx.stream);
		soundio_disconnect(ctx.io);
		soundio_destroy(ctx.io);
		exit(EXIT_FAILURE);
	}

	ctx.rb = soundio_ring_buffer_create(ctx.io,
		BUF_TIME_S * ctx.stream->sample_rate * ctx.stream->bytes_per_frame);
	if (ctx.rb == NULL) {
		fprintf(stderr, "\rCouldn't allocate ring buffer for audio\r");
		soundio_device_unref(ctx.dev);
		soundio_instream_destroy(ctx.stream);
		soundio_disconnect(ctx.io);
		soundio_destroy(ctx.io);
		exit(EXIT_FAILURE);
	}

	err = soundio_instream_start(ctx.stream);
	if (err) {
		fprintf(stderr, "Error recording: %s\n", soundio_strerror(err));
		exit(EXIT_FAILURE);
	}

	recording = 1;

	if (pthread_create(&wthread, NULL, writer, NULL) != 0) {
		perror("pthread_create");
		soundio_device_unref(ctx.dev);
		soundio_instream_destroy(ctx.stream);
		soundio_disconnect(ctx.io);
		soundio_destroy(ctx.io);
		xfclose(ctx.fout);
		exit(EXIT_FAILURE);
	}

	if (pthread_create(&rthread, NULL, reader, NULL) != 0) {
		perror("pthread_create");
		soundio_device_unref(ctx.dev);
		soundio_instream_destroy(ctx.stream);
		soundio_disconnect(ctx.io);
		soundio_destroy(ctx.io);
		xfclose(ctx.fout);
		exit(EXIT_FAILURE);
	}

	while (post) {
		usleep(10);
	}
}

void
audio_init(const char *devid, const char *outfile)
{

	ctx.active = 1;
	ctx.fout = xfopen(outfile, "wb");
	ctx.devid = devid;
}

void
audio_stop(void)
{

	if (!ctx.active) {
		return;
	}

	if (ctx.io) {
		soundio_flush_events(ctx.io);
		usleep(100);
	}

	post = 2;

	pthread_join(wthread, NULL);
	pthread_join(rthread, NULL);

	if (ctx.stream) {
		soundio_instream_destroy(ctx.stream);
	}

	if (ctx.dev) {
		soundio_device_unref(ctx.dev);
	}

	if (ctx.io) {
		soundio_destroy(ctx.io);
	}
}

void
audio_exit(void)
{

	if (ctx.fout) {
		xfclose(ctx.fout);
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

		printf("%4d: %s %dHz\n"
		    "      castty -d '%s' -a audio.%s\n", i, dev->name, rate,
		    dev->id, soundio_format_string(fmt));
		soundio_device_unref(dev);
	}

	soundio_disconnect(soundio);
	soundio_destroy(soundio);

	exit(EXIT_SUCCESS);
}

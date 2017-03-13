#define _BSD_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <pthread.h>
#include <unistd.h>

#include <soundio/soundio.h>

#include "castty.h"

#ifndef MIN
#define MIN(a, b) ((a < b) ? a : b)
#endif

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
	SoundIoFormatS8,
	SoundIoFormatU8,
	SoundIoFormatInvalid,
};

static const char *
stringify_format(enum SoundIoFormat fmt)
{

	switch (fmt) {
        case SoundIoFormatFloat32LE:
		return "f32le";
        case SoundIoFormatFloat32BE:
		return "f32be";
        case SoundIoFormatU32BE:
		return "u32be";
        case SoundIoFormatU32LE:
		return "u32le";
        case SoundIoFormatS32BE:
		return "s32be";
        case SoundIoFormatS32LE:
		return "s32le";
        case SoundIoFormatS24BE:
		return "s24be";
        case SoundIoFormatS24LE:
		return "s24le";
        case SoundIoFormatU24BE:
		return "u24be";
        case SoundIoFormatU24LE:
		return "u24le";
        case SoundIoFormatS16BE:
		return "s16be";
        case SoundIoFormatS16LE:
		return "s16le";
        case SoundIoFormatU16BE:
		return "u16be";
        case SoundIoFormatU16LE:
		return "u16le";
        case SoundIoFormatS8:
		return "s8";
        case SoundIoFormatU8:
		return "u8";
	default:
		return "invalid";
	}
}

static int rates[] = {
	44100,
	24000,
	48000,
	96000,
	128000,
	0,
};

static struct audio_ctx {
	FILE *fout;
	double clock;

	struct SoundIoInStream *stream;
	struct SoundIoRingBuffer *rb;
	struct SoundIoDevice *dev;
	struct SoundIo *io;
} ctx;

static int post = 2;
static int recording;
static int muted;

pthread_t wthread, rthread;

static void *
writer(void *priv)
{
	(void)priv;

	__sync_fetch_and_sub(&post, 1);
	while (post > 0 || recording == 0) {
		usleep(10);
	}

	while (1) {
		int fill_bytes = soundio_ring_buffer_fill_count(ctx.rb);
		char *read_buf = soundio_ring_buffer_read_ptr(ctx.rb);

		if (recording) {
			size_t amt = fwrite(read_buf, 1, fill_bytes, ctx.fout);
			if ((int)amt != fill_bytes) {
				perror("fwrite");
				exit(EXIT_FAILURE);
			}
		}
		soundio_ring_buffer_advance_read_ptr(ctx.rb, fill_bytes);

		if (post > 0) {
			break;
		}
	}

	return NULL;
}

double
audio_clock_ms(void)
{

	return (double)(ctx.clock * 1000.) / (double)ctx.stream->sample_rate;
}

static void
audio_record(struct SoundIoInStream *instream, int frame_count_min, int frame_count_max)
{
	struct SoundIoChannelArea *areas;
	int err;

	char *write_ptr = soundio_ring_buffer_write_ptr(ctx.rb);
	int free_bytes = soundio_ring_buffer_free_count(ctx.rb);
	int free_count = free_bytes / instream->bytes_per_frame;

	if (free_count < frame_count_min) {
		fprintf(stderr, "ring buffer overflow\n");
		exit(EXIT_FAILURE);
	}

	int write_frames = MIN(free_count, frame_count_max);
	int frames_left = write_frames;

	if (!recording) {
		int advance_bytes = write_frames * instream->bytes_per_frame;
		soundio_ring_buffer_advance_write_ptr(ctx.rb, advance_bytes);
		return;
	}

	while (1) {
		int frame_count = frames_left;

		if ((err = soundio_instream_begin_read(instream, &areas, &frame_count))) {
			fprintf(stderr, "begin read error: %s", soundio_strerror(err));
			exit(EXIT_FAILURE);
		}

		if (!frame_count)
			break;

		if (!areas) {
			// Due to an overflow there is a hole. Fill the ring buffer with
			// silence for the size of the hole.
			memset(write_ptr, 0, frame_count * instream->bytes_per_frame);
		} else {
			for (int frame = 0; frame < frame_count; frame += 1) {
				ctx.clock++;

				for (int ch = 0; ch < instream->layout.channel_count; ch += 1) {
					if (muted) {
						memset(write_ptr, 0, instream->bytes_per_sample);
					} else {
						memcpy(write_ptr, areas[ch].ptr, instream->bytes_per_sample);
					}

					areas[ch].ptr += areas[ch].step;
					write_ptr += instream->bytes_per_sample;
				}
			}
		}

		if ((err = soundio_instream_end_read(instream))) {
			fprintf(stderr, "end read error: %s", soundio_strerror(err));
			exit(EXIT_FAILURE);
		}

		frames_left -= frame_count;
		if (frames_left <= 0)
			break;
	}

	int advance_bytes = write_frames * instream->bytes_per_frame;
	soundio_ring_buffer_advance_write_ptr(ctx.rb, advance_bytes);
}

static void *
reader(void *priv)
{
	int err;

	(void)priv;

	__sync_fetch_and_sub(&post, 1);
	while (post > 0 || recording == 0) {
		usleep(10);
	}

	err = soundio_instream_start(ctx.stream);
	if (err) {
		fprintf(stderr, "Error recording: %s\n", soundio_strerror(err));
		exit(EXIT_FAILURE);
	}

	while (1) {
		soundio_flush_events(ctx.io);
		usleep(1000);

		if (post > 0) {
			soundio_flush_events(ctx.io);
			usleep(10000);
			break;
		}
	}

	return NULL;
}

void
audio_toggle_pause(void)
{

	recording = !recording;
}

void
audio_toggle_mute(void)
{

	muted = !muted;
}

void
audio_start(const char *devid, const char *outfile, int append)
{
	struct SoundIo *soundio;
	FILE *fout;
	int err;

	fout = xfopen(outfile, append ? "ab" : "wb");

	soundio = soundio_create();
	if (soundio == NULL) {
		fprintf(stderr, "Couldn't initialize audio\n");
		fclose(fout);
		exit(EXIT_FAILURE);
	}

	if ((err = soundio_connect(soundio))) {
		fprintf(stderr, "Couldn't connect to audio backend: %s",
		    soundio_strerror(err));
		fclose(fout);
		exit(EXIT_FAILURE);
	}

	soundio_flush_events(soundio);

	int ndev = soundio_input_device_count(soundio);
	if (ndev == 0) {
		fprintf(stderr, "No input devices available.\n");
		soundio_disconnect(soundio);
		soundio_destroy(soundio);
		fclose(fout);
		exit(EXIT_FAILURE);
	}

	struct SoundIoDevice *dev = NULL;
	for (int i = 0; i < ndev; i++) {
		dev = soundio_get_input_device(soundio, i);
		if (!strcmp(dev->id, devid)) {
			break;
		}

		soundio_device_unref(dev);
		dev = NULL;
	}

	if (dev == NULL) {
		fprintf(stderr, "Couldn't find requested device\n");
		soundio_disconnect(soundio);
		soundio_destroy(soundio);
		xfclose(fout);
		exit(EXIT_FAILURE);
	}

	if (dev->probe_error) {
		fprintf(stderr, "Device error while probing: %s\n",
		    soundio_strerror(dev->probe_error));
		soundio_disconnect(soundio);
		soundio_destroy(soundio);
		xfclose(fout);
		exit(EXIT_FAILURE);
	}

	soundio_device_sort_channel_layouts(dev);

	ctx.stream = soundio_instream_create(dev);
	if (ctx.stream == NULL) {
		fprintf(stderr, "Couldn't create stream\n");
		soundio_disconnect(soundio);
		soundio_destroy(soundio);
		xfclose(fout);
		exit(EXIT_FAILURE);
	}

	for (unsigned i = 0; i < sizeof formats / sizeof formats[0]; i++) {
		if (formats[i] == SoundIoFormatInvalid) {
			fprintf(stderr, "Input device supports no usable formats\n");
			soundio_instream_destroy(ctx.stream);
			soundio_disconnect(soundio);
			soundio_destroy(soundio);
			xfclose(fout);
			exit(EXIT_FAILURE);
		}

		if (soundio_device_supports_format(dev, formats[i])) {
			ctx.stream->format = formats[i];
			break;
		}
	}

	for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) {
		if (rates[i] == 0) {
			fprintf(stderr, "Input device supports no usable rates\n");
			soundio_instream_destroy(ctx.stream);
			soundio_disconnect(soundio);
			soundio_destroy(soundio);
			xfclose(fout);
			exit(EXIT_FAILURE);
		}

		if (soundio_device_supports_sample_rate(dev, rates[i])) {
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
		soundio_instream_destroy(ctx.stream);
		soundio_disconnect(soundio);
		soundio_destroy(soundio);
		xfclose(fout);
		exit(EXIT_FAILURE);
	}

	ctx.fout = fout;
	ctx.io = soundio;
	ctx.dev = dev;
	ctx.rb = soundio_ring_buffer_create(soundio,
	    60 * 44100 * ctx.stream->bytes_per_frame);
	
	if (ctx.rb == NULL) {
		fprintf(stderr, "Couldn't allocate ring buffer for audio\n");
		soundio_instream_destroy(ctx.stream);
		soundio_disconnect(soundio);
		soundio_destroy(soundio);
		xfclose(fout);
		exit(EXIT_FAILURE);
	}

	if (pthread_create(&wthread, NULL, writer, NULL) != 0) {
		perror("pthread_create");
		exit(EXIT_FAILURE);
	}

	if (pthread_create(&rthread, NULL, reader, NULL) != 0) {
		perror("pthread_create");
		exit(EXIT_FAILURE);
	}

	while (post) {
		usleep(1000);
	}
}

void
audio_stop(void)
{

	post++;

	pthread_join(wthread, NULL);
	pthread_join(rthread, NULL);

	soundio_instream_destroy(ctx.stream);
	soundio_disconnect(ctx.io);
	soundio_destroy(ctx.io);
	xfclose(ctx.fout);
}

void
audio_list(void)
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
		struct SoundIoDevice *dev;
		enum SoundIoFormat fmt;
		int rate;

		dev = soundio_get_input_device(soundio, i);
		if (dev->probe_error) {
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
		    dev->id, stringify_format(fmt));
		soundio_device_unref(dev);
	}

	soundio_disconnect(soundio);
	soundio_destroy(soundio);

	exit(EXIT_SUCCESS);
}

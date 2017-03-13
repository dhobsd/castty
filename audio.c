#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <pthread.h>
#include <unistd.h>

#include <lame/lame.h>
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
	SoundIoFormatInvalid,
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
	FILE *fout;
	double clock;
	int mono;

	struct SoundIoInStream *stream;
	struct SoundIoRingBuffer *rb;
	struct SoundIoDevice *dev;
	struct SoundIo *io;
} ctx;

static int post = 2;
static int recording;
static int muted;
static int mp3;

pthread_t wthread, rthread;

enum {
	/* 4MB should be good enough for anybody ;P */
	MP3_BUF_SIZE = 1<<22,
};

static void *
writer(void *priv)
{
	unsigned char *mp3buf = NULL;
	lame_t lame;

	(void)priv;

	__sync_fetch_and_sub(&post, 1);
	while (post > 0 || recording == 0) {
		usleep(10);
	}

	if (mp3) {
		lame = lame_init();
		if (lame == NULL) {
			fprintf(stderr, "Couldn't initialize lame encoder\n");
			exit(EXIT_FAILURE);
		}

		mp3buf = malloc(MP3_BUF_SIZE);
		if (mp3buf == NULL) {
			fprintf(stderr, "No memory for mp3 encoding buffer\n");
			exit(EXIT_FAILURE);
		}

		lame_set_num_channels(lame, ctx.stream->layout.channel_count);
		lame_set_mode(lame, ctx.mono ? MONO : STEREO);
		lame_set_error_protection(lame, 1);
		lame_set_in_samplerate(lame, ctx.stream->sample_rate);
		lame_set_findReplayGain(lame, 1);
		lame_set_asm_optimizations(lame, MMX, 1);
		lame_set_asm_optimizations(lame, SSE, 1);
		lame_set_quality(lame, 3);
		lame_set_bWriteVbrTag(lame, 1);
		lame_set_VBR(lame, vbr_mtrh);
		lame_set_VBR_q(lame, 3);
		lame_set_VBR_min_bitrate_kbps(lame, 96);
		lame_set_VBR_max_bitrate_kbps(lame, 320);

		lame_init_params(lame);
	}

	while (1) {
		int fill_bytes = soundio_ring_buffer_fill_count(ctx.rb);
		char *read_buf = soundio_ring_buffer_read_ptr(ctx.rb);

		if (recording) {
			if (mp3) {
				int blen;

				switch (ctx.stream->format) {
				case SoundIoFormatFloat32LE:
				case SoundIoFormatFloat32BE:
					blen = lame_encode_buffer_ieee_float(lame,
					    (float *)read_buf,
					    (float *)(read_buf + (fill_bytes / 2)),
					    fill_bytes / ctx.stream->bytes_per_frame,
					    mp3buf, MP3_BUF_SIZE);
					break;
				case SoundIoFormatU32BE:
				case SoundIoFormatU32LE:
					blen = lame_encode_buffer_long2(lame,
					    (long *)read_buf,
					    (long *)(read_buf + (fill_bytes / 2)),
					    fill_bytes / ctx.stream->bytes_per_frame,
					    mp3buf, MP3_BUF_SIZE);
					break;

				case SoundIoFormatS32BE:
				case SoundIoFormatS32LE:
				case SoundIoFormatS24BE:
				case SoundIoFormatS24LE:
				case SoundIoFormatU24BE:
				case SoundIoFormatU24LE:
					blen = lame_encode_buffer_int(lame,
					    (int *)read_buf,
					    (int *)(read_buf + (fill_bytes / 2)),
					    fill_bytes / ctx.stream->bytes_per_frame,
					    mp3buf, MP3_BUF_SIZE);
					break;

				case SoundIoFormatS16BE:
				case SoundIoFormatS16LE:
				case SoundIoFormatU16BE:
				case SoundIoFormatU16LE:
					blen = lame_encode_buffer(lame,
					    (short *)read_buf,
					    (short *)(read_buf + (fill_bytes / 2)),
					    fill_bytes / ctx.stream->bytes_per_frame,
					    mp3buf, MP3_BUF_SIZE);
					break;
				default:
					fprintf(stderr, "Invalid format!\n");
					exit(EXIT_FAILURE);
				}

				fwrite(mp3buf, 1, blen, ctx.fout);
			} else {
				size_t amt = fwrite(read_buf, 1, fill_bytes, ctx.fout);
				if ((int)amt != fill_bytes) {
					perror("fwrite");
					exit(EXIT_FAILURE);
				}
			}
		}
		soundio_ring_buffer_advance_read_ptr(ctx.rb, fill_bytes);

		if (post > 0) {
			break;
		}
	}

	int blen = lame_encode_flush(lame, mp3buf, MP3_BUF_SIZE);
	fwrite(mp3buf, 1, blen, ctx.fout);

	lame_close(lame);

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
			if (muted) {
				memset(write_ptr, 0, instream->bytes_per_sample *
				    instream->layout.channel_count * frame_count);
				ctx.clock += frame_count;
			} else {
				/* Don't interleave when outputting MP3 */
				if (!mp3) {
					for (int frame = 0; frame < frame_count; frame++) {
						int ch = 0;
						memcpy(write_ptr, areas[ch].ptr, instream->bytes_per_sample);

						if (!ctx.mono) {
							areas[ch].ptr += areas[ch].step;
							ch++;
						}
						write_ptr += instream->bytes_per_sample;

						memcpy(write_ptr, areas[ch].ptr, instream->bytes_per_sample);
						write_ptr += instream->bytes_per_sample;
						areas[ch].ptr += areas[ch].step;
						ctx.clock++;
					}
				} else {
					int off = frame_count * instream->bytes_per_sample;
					for (int frame = 0; frame < frame_count; frame++) {
						int ch = 0;
						memcpy(write_ptr, areas[ch].ptr, instream->bytes_per_sample);

						if (!ctx.mono) {
							areas[ch].ptr += areas[ch].step;
							ch++;
						}

						memcpy(write_ptr + off, areas[ch].ptr, instream->bytes_per_sample);

						write_ptr += instream->bytes_per_sample;
						areas[ch].ptr += areas[ch].step;
						ctx.clock++;
					}
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
audio_toggle_mp3(void)
{

	mp3 = !mp3;
}

void
audio_toggle_mute(void)
{

	muted = !muted;
}

void
audio_start(const char *devid, const char *outfile, int append)
{
	const struct SoundIoChannelLayout *stereo, *mono;
	struct SoundIo *soundio;
	FILE *fout;
	int err;

	fout = xfopen(outfile, append ? "ab" : "wb");

	stereo = soundio_channel_layout_get_builtin(SoundIoChannelLayoutIdStereo);
	mono = soundio_channel_layout_get_builtin(SoundIoChannelLayoutIdMono);

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

	if (soundio_device_supports_layout(dev, stereo)) {
		ctx.stream->layout = *stereo;
		ctx.mono = 0;
	} else if (soundio_device_supports_layout(dev, mono)) {
		ctx.stream->layout = *mono;
		ctx.mono = 1;
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
	    60 * ctx.stream->sample_rate * ctx.stream->bytes_per_frame);
	
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

#include <assert.h>
#include <lame/lame.h>
#include <soundio/soundio.h>
#include <stdio.h>
#include <stdlib.h>

#include "writer-lame.h"
#include "writer.h"

enum {
	MP3_BUF_SIZE = 1UL << 22,
};

struct lame {
	FILE *outfile;
	lame_t lflags;
	unsigned char *buf;
	size_t buf_size;
};

static void
lame_write(struct audio_writer *writer, enum SoundIoFormat fmt, char *data, int size,
    int bytes_per_frame)
{
	int nsamples, blen = 0;
	char *left, *right;
	struct lame *lame;

	assert(writer != NULL);
	assert(data != NULL);

	lame = writer->context;

	left = data;
	right = data + size / 2;
	nsamples = size / bytes_per_frame;

	switch (fmt) {
	case SoundIoFormatFloat32LE:
	case SoundIoFormatFloat32BE:
		blen = lame_encode_buffer_ieee_float(lame->lflags,
		    (float *)left, (float *)right, nsamples,
		    lame->buf, lame->buf_size);
		break;

	case SoundIoFormatU32BE:
	case SoundIoFormatU32LE:
		blen = lame_encode_buffer_long2(lame->lflags,
		    (long *)left, (long *)right, nsamples,
		    lame->buf, lame->buf_size);
		break;

	case SoundIoFormatS32BE:
	case SoundIoFormatS32LE:
	case SoundIoFormatS24BE:
	case SoundIoFormatS24LE:
	case SoundIoFormatU24BE:
	case SoundIoFormatU24LE:
		blen = lame_encode_buffer_int(lame->lflags,
		    (int *)left, (int *)right, nsamples,
		    lame->buf, lame->buf_size);
		break;

	case SoundIoFormatS16BE:
	case SoundIoFormatS16LE:
	case SoundIoFormatU16BE:
	case SoundIoFormatU16LE:
		blen = lame_encode_buffer(lame->lflags,
		     (short *)left, (short *)right, nsamples,
		     lame->buf, lame->buf_size);
		break;
	default:
		fprintf(stderr, "Invalid format\n");
		exit(EXIT_FAILURE);
	}

	fwrite(lame->buf, 1, blen, lame->outfile);
}

static void
lame_destroy(struct audio_writer *writer)
{
	struct lame *lame;
	int blen;

	assert(writer);
	lame = writer->context;

	blen = lame_encode_flush(lame->lflags, lame->buf, lame->buf_size);
	fwrite(lame->buf, 1, blen, lame->outfile);

	lame_close(lame->lflags);
	free(lame->buf);
	free(lame);
	free(writer);
}

struct audio_writer *
audio_writer_lame(FILE *outfile, int sample_rate, int nchannels, int buf_time_s, int mono)
{
	struct audio_writer *writer;
	struct lame *lame;

	assert(outfile != NULL);
	assert(sample_rate > 0);
	assert(nchannels > 0);
	assert(buf_time_s >= 1);

	writer = malloc(sizeof *writer);
	if (!writer) {
		fprintf(stderr, "No memory for audio writer\n");
		exit(EXIT_FAILURE);
	}

	lame = malloc(sizeof *lame);
	if (!lame) {
		fprintf(stderr, "No memory for LAME audio writer\n");
		exit(EXIT_FAILURE);
	}

	lame->outfile = outfile;

	lame->lflags = lame_init();
	if (lame->lflags == NULL) {
		fprintf(stderr, "Couldn't initialize lame encoder\n");
		exit(EXIT_FAILURE);
	}

	lame->buf_size = 1.25 * buf_time_s * sample_rate + 7200;
	lame->buf = malloc(MP3_BUF_SIZE);
	if (lame->buf == NULL) {
		fprintf(stderr, "No memory for mp3 buffer\n");
		exit(EXIT_FAILURE);
	}

	lame_set_num_channels(lame->lflags, nchannels);
	lame_set_mode(lame->lflags, mono ? MONO : STEREO);
	lame_set_error_protection(lame->lflags, 1);
	lame_set_in_samplerate(lame->lflags, sample_rate);
	lame_set_findReplayGain(lame->lflags, 1);
	lame_set_asm_optimizations(lame->lflags, MMX, 1);
	lame_set_asm_optimizations(lame->lflags, SSE, 1);
	lame_set_quality(lame->lflags, 3);
	lame_set_bWriteVbrTag(lame->lflags, 1);
	lame_set_VBR(lame->lflags, vbr_mtrh);
	lame_set_VBR_q(lame->lflags, 3);
	lame_set_VBR_min_bitrate_kbps(lame->lflags, 96);
	lame_set_VBR_max_bitrate_kbps(lame->lflags, 320);

	lame_init_params(lame->lflags);

	writer->context = lame;
	writer->write = lame_write;
	writer->destroy = lame_destroy;

	return writer;
}

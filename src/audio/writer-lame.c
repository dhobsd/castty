#include <assert.h>
#include <lame/lame.h>
#include <soundio/soundio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/writer-lame.h"
#include "audio/writer.h"

struct lame {
	FILE *outfile;
	lame_t lflags;
	unsigned char *buf;
	size_t buf_size;
	float *lbuf;
	float *rbuf;
	int frame_cap;
	int nchannels;
};

static uint16_t
read_u16le(const unsigned char *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint16_t
read_u16be(const unsigned char *p)
{
	return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t
read_u32le(const unsigned char *p)
{
	return (uint32_t)p[0] |
	    ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) |
	    ((uint32_t)p[3] << 24);
}

static uint32_t
read_u32be(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) |
	    ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) |
	    (uint32_t)p[3];
}

static uint64_t
read_u64le(const unsigned char *p)
{
	return (uint64_t)read_u32le(p) | ((uint64_t)read_u32le(p + 4) << 32);
}

static uint64_t
read_u64be(const unsigned char *p)
{
	return ((uint64_t)read_u32be(p) << 32) | (uint64_t)read_u32be(p + 4);
}

static float
decode_s24(uint32_t w)
{
	uint32_t u = w & 0xffffffu;
	int32_t s = (int32_t)(u ^ 0x800000u) - 0x800000;

	return (float)s / 256.0f;
}

static float
decode_u24(uint32_t w)
{
	uint32_t u = w & 0xffffffu;

	return ((float)u - 8388608.0f) / 256.0f;
}

static float
decode_s32(uint32_t u)
{
	int32_t s;

	memcpy(&s, &u, sizeof s);
	return (float)((double)s / 65536.0);
}

static float
decode_u32(uint32_t u)
{
	return (float)(((double)u - 2147483648.0) / 65536.0);
}

static float
decode_f32(uint32_t u)
{
	float f;

	memcpy(&f, &u, sizeof f);
	return f * 32768.0f;
}

static float
decode_f64(uint64_t u)
{
	double d;

	memcpy(&d, &u, sizeof d);
	return (float)(d * 32768.0);
}

static int
format_sample_bytes(enum SoundIoFormat fmt)
{
	switch (fmt) {
	case SoundIoFormatS8:
	case SoundIoFormatU8:
		return 1;
	case SoundIoFormatS16LE:
	case SoundIoFormatS16BE:
	case SoundIoFormatU16LE:
	case SoundIoFormatU16BE:
		return 2;
	case SoundIoFormatS24LE:
	case SoundIoFormatS24BE:
	case SoundIoFormatU24LE:
	case SoundIoFormatU24BE:
	case SoundIoFormatS32LE:
	case SoundIoFormatS32BE:
	case SoundIoFormatU32LE:
	case SoundIoFormatU32BE:
	case SoundIoFormatFloat32LE:
	case SoundIoFormatFloat32BE:
		return 4;
	case SoundIoFormatFloat64LE:
	case SoundIoFormatFloat64BE:
		return 8;
	default:
		return 0;
	}
}

static float
decode_sample(enum SoundIoFormat fmt, const unsigned char *p)
{
	switch (fmt) {
	case SoundIoFormatS8: {
		int8_t s;

		memcpy(&s, p, sizeof s);
		return (float)s * 256.0f;
	}
	case SoundIoFormatU8:
		return ((float)p[0] - 128.0f) * 256.0f;
	case SoundIoFormatS16LE: {
		uint16_t u = read_u16le(p);
		int16_t s;

		memcpy(&s, &u, sizeof s);
		return (float)s;
	}
	case SoundIoFormatS16BE: {
		uint16_t u = read_u16be(p);
		int16_t s;

		memcpy(&s, &u, sizeof s);
		return (float)s;
	}
	case SoundIoFormatU16LE:
		return (float)read_u16le(p) - 32768.0f;
	case SoundIoFormatU16BE:
		return (float)read_u16be(p) - 32768.0f;
	case SoundIoFormatS24LE:
		return decode_s24(read_u32le(p));
	case SoundIoFormatS24BE:
		return decode_s24(read_u32be(p));
	case SoundIoFormatU24LE:
		return decode_u24(read_u32le(p));
	case SoundIoFormatU24BE:
		return decode_u24(read_u32be(p));
	case SoundIoFormatS32LE:
		return decode_s32(read_u32le(p));
	case SoundIoFormatS32BE:
		return decode_s32(read_u32be(p));
	case SoundIoFormatU32LE:
		return decode_u32(read_u32le(p));
	case SoundIoFormatU32BE:
		return decode_u32(read_u32be(p));
	case SoundIoFormatFloat32LE:
		return decode_f32(read_u32le(p));
	case SoundIoFormatFloat32BE:
		return decode_f32(read_u32be(p));
	case SoundIoFormatFloat64LE:
		return decode_f64(read_u64le(p));
	case SoundIoFormatFloat64BE:
		return decode_f64(read_u64be(p));
	default:
		fprintf(stderr, "Invalid format\n");
		exit(EXIT_FAILURE);
	}
}

static void
lame_write(struct audio_writer *writer, enum SoundIoFormat fmt, char *data, int size,
    int bytes_per_frame)
{
	struct lame *lame;
	const unsigned char *src;
	int nsamples, sample_bytes, offset;

	assert(writer != NULL);
	assert(data != NULL);
	assert(bytes_per_frame > 0);

	if (size <= 0) {
		return;
	}

	lame = writer->context;
	sample_bytes = format_sample_bytes(fmt);
	if (sample_bytes == 0 || bytes_per_frame != lame->nchannels * sample_bytes) {
		fprintf(stderr, "Invalid audio format or frame size\n");
		exit(EXIT_FAILURE);
	}

	nsamples = size / bytes_per_frame;
	if (nsamples <= 0) {
		return;
	}

	src = (const unsigned char *)data;
	for (offset = 0; offset < nsamples;) {
		int chunk = nsamples - offset;
		int i, blen;

		if (chunk > lame->frame_cap) {
			chunk = lame->frame_cap;
		}
		for (i = 0; i < chunk; i++) {
			const unsigned char *frame = src + (size_t)bytes_per_frame * i;

			lame->lbuf[i] = decode_sample(fmt, frame);
			lame->rbuf[i] = lame->nchannels > 1 ?
			    decode_sample(fmt, frame + sample_bytes) : lame->lbuf[i];
		}

		blen = lame_encode_buffer_float(lame->lflags, lame->lbuf,
		    lame->rbuf, chunk, lame->buf, (int)lame->buf_size);
		if (blen < 0) {
			fprintf(stderr, "lame_encode_buffer_float failed: %d\n", blen);
			exit(EXIT_FAILURE);
		}
		if (blen > 0 &&
		    fwrite(lame->buf, 1, (size_t)blen, lame->outfile) != (size_t)blen) {
			perror("fwrite");
			exit(EXIT_FAILURE);
		}

		src += (size_t)chunk * bytes_per_frame;
		offset += chunk;
	}
}

static void
lame_destroy(struct audio_writer *writer)
{
	struct lame *lame;
	int blen;

	assert(writer);
	lame = writer->context;

	blen = lame_encode_flush(lame->lflags, lame->buf, (int)lame->buf_size);
	if (blen > 0 &&
	    fwrite(lame->buf, 1, (size_t)blen, lame->outfile) != (size_t)blen) {
		perror("fwrite");
	} else if (blen < 0) {
		fprintf(stderr, "lame_encode_flush failed: %d\n", blen);
	}

	lame_close(lame->lflags);
	free(lame->lbuf);
	free(lame->rbuf);
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
	assert(nchannels == 1 || nchannels == 2);
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
	lame->nchannels = nchannels;

	lame->lflags = lame_init();
	if (lame->lflags == NULL) {
		fprintf(stderr, "Couldn't initialize lame encoder\n");
		exit(EXIT_FAILURE);
	}

	lame->buf_size = 1.25 * buf_time_s * sample_rate + 7200;
	lame->buf = malloc(lame->buf_size);
	if (lame->buf == NULL) {
		fprintf(stderr, "No memory for mp3 buffer\n");
		exit(EXIT_FAILURE);
	}

	lame->frame_cap = buf_time_s * sample_rate;
	lame->lbuf = calloc(lame->frame_cap, sizeof *lame->lbuf);
	lame->rbuf = calloc(lame->frame_cap, sizeof *lame->rbuf);
	if (!lame->lbuf || !lame->rbuf) {
		fprintf(stderr, "No memory for LAME channel buffers\n");
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

#include <assert.h>
#include <fcntl.h>
#include <lame/lame.h>
#include <soundio/soundio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "audio/pcm.h"
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
	sample_bytes = pcm_sample_bytes(fmt);
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

			lame->lbuf[i] = pcm_decode_sample(fmt, frame);
			lame->rbuf[i] = lame->nchannels > 1 ?
			    pcm_decode_sample(fmt, frame + sample_bytes) : lame->lbuf[i];
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
	int blen, fd, fl;

	assert(writer);
	lame = writer->context;

	blen = lame_encode_flush(lame->lflags, lame->buf, (int)lame->buf_size);
	if (blen > 0 &&
	    fwrite(lame->buf, 1, (size_t)blen, lame->outfile) != (size_t)blen) {
		perror("fwrite");
	} else if (blen < 0) {
		fprintf(stderr, "lame_encode_flush failed: %d\n", blen);
	}

	fflush(lame->outfile);
	fd = fileno(lame->outfile);
	fl = (fd >= 0) ? fcntl(fd, F_GETFL) : -1;
	if (fl != -1 && (fl & O_ACCMODE) == O_RDWR &&
	    lseek(fd, 0, SEEK_CUR) >= 0) {
		lame_mp3_tags_fid(lame->lflags, lame->outfile);
		fflush(lame->outfile);
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

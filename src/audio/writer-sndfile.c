#include <assert.h>
#include <sndfile.h>
#include <soundio/soundio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "audio/pcm.h"
#include "audio/writer-sndfile.h"
#include "audio/writer.h"

enum {
	SNDFILE_CHUNK_FRAMES = 4096,
};

struct sf_writer {
	SNDFILE *handle;
	int fd;
	float *fbuf;
	int frame_cap;
	int in_channels;
	int out_channels;
};

int
audio_sndfile_format(enum audio_format fmt)
{
	switch (fmt) {
	case AUDIO_FMT_WAV:
		return SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	case AUDIO_FMT_FLAC:
		return SF_FORMAT_FLAC | SF_FORMAT_PCM_16;
	case AUDIO_FMT_OPUS:
		return SF_FORMAT_OGG | SF_FORMAT_OPUS;
	case AUDIO_FMT_OGG:
		return SF_FORMAT_OGG | SF_FORMAT_VORBIS;
	default:
		return 0;
	}
}

static void
sndfile_write(struct audio_writer *writer, enum SoundIoFormat fmt, char *data,
    int size, int bytes_per_frame)
{
	struct sf_writer *sf;
	const unsigned char *src;
	int nsamples, sample_bytes, offset;

	assert(writer != NULL);
	assert(data != NULL);
	assert(bytes_per_frame > 0);

	if (size <= 0) {
		return;
	}

	sf = writer->context;
	sample_bytes = pcm_sample_bytes(fmt);
	if (sample_bytes == 0 || bytes_per_frame != sf->in_channels * sample_bytes) {
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
		int i;
		sf_count_t nwritten;

		if (chunk > sf->frame_cap) {
			chunk = sf->frame_cap;
		}
		for (i = 0; i < chunk; i++) {
			const unsigned char *frame = src + (size_t)bytes_per_frame * i;
			float l = pcm_decode_sample(fmt, frame) / 32768.0f;
			float r = (sf->in_channels > 1) ?
			    (pcm_decode_sample(fmt, frame + sample_bytes) / 32768.0f) : l;

			if (sf->out_channels == 1) {
				sf->fbuf[i] = 0.5f * (l + r);
			} else {
				sf->fbuf[i * 2] = l;
				sf->fbuf[i * 2 + 1] = r;
			}
		}

		nwritten = sf_writef_float(sf->handle, sf->fbuf, chunk);
		if (nwritten != chunk) {
			fprintf(stderr, "sf_writef_float failed: %s\n",
			    sf_strerror(sf->handle));
			exit(EXIT_FAILURE);
		}

		src += (size_t)chunk * bytes_per_frame;
		offset += chunk;
	}
}

static void
sndfile_destroy(struct audio_writer *writer)
{
	struct sf_writer *sf;
	int err;

	assert(writer != NULL);
	sf = writer->context;

	if (sf->handle != NULL) {
		err = sf_close(sf->handle);
		if (err != 0) {
			fprintf(stderr, "sf_close failed: %s\n",
			    sf_error_number(err));
		}
	}
	if (sf->fd >= 0) {
		close(sf->fd);
	}
	free(sf->fbuf);
	free(sf);
	free(writer);
}

/* Open a libsndfile-backed writer wrapping a duplicated file descriptor so sf_close
 * updates container headers without colliding with stdio fclose() on outfile. */
struct audio_writer *
audio_writer_sndfile(FILE *outfile, enum audio_format fmt, int sample_rate,
    int nchannels, int buf_time_s, int mono)
{
	struct audio_writer *writer;
	struct sf_writer *sf;
	SF_INFO sfinfo;
	int sf_fmt, fd, out_ch;

	assert(outfile != NULL);
	assert(sample_rate > 0);
	assert(nchannels == 1 || nchannels == 2);
	assert(buf_time_s >= 1);

	sf_fmt = audio_sndfile_format(fmt);
	if (sf_fmt == 0) {
		return NULL;
	}

	out_ch = mono ? 1 : nchannels;
	sfinfo = (SF_INFO){
		.samplerate = sample_rate,
		.channels = out_ch,
		.format = sf_fmt,
	};
	if (!sf_format_check(&sfinfo)) {
		fprintf(stderr,
		    "Unsupported audio parameters for format (%d Hz, %d ch)\n",
		    sample_rate, out_ch);
		return NULL;
	}

	if (fflush(outfile) != 0) {
		perror("fflush");
		return NULL;
	}
	fd = dup(fileno(outfile));
	if (fd < 0) {
		perror("dup");
		return NULL;
	}
	if (lseek(fd, 0, SEEK_SET) < 0) {
		perror("lseek");
		close(fd);
		return NULL;
	}

	writer = malloc(sizeof *writer);
	sf = malloc(sizeof *sf);
	if (writer == NULL || sf == NULL) {
		fprintf(stderr, "No memory for sndfile audio writer\n");
		free(writer);
		free(sf);
		close(fd);
		exit(EXIT_FAILURE);
	}

	sf->frame_cap = SNDFILE_CHUNK_FRAMES;
	sf->fbuf = calloc((size_t)sf->frame_cap * (size_t)out_ch,
	    sizeof *sf->fbuf);
	if (sf->fbuf == NULL) {
		fprintf(stderr, "No memory for sndfile buffer\n");
		free(sf);
		free(writer);
		close(fd);
		exit(EXIT_FAILURE);
	}

	sf->handle = sf_open_fd(fd, SFM_WRITE, &sfinfo, SF_FALSE);
	if (sf->handle == NULL) {
		fprintf(stderr, "sf_open_fd failed: %s\n", sf_strerror(NULL));
		free(sf->fbuf);
		free(sf);
		free(writer);
		close(fd);
		return NULL;
	}

	(void)sf_command(sf->handle, SFC_SET_CLIPPING, NULL, SF_TRUE);
	if (fmt == AUDIO_FMT_OGG || fmt == AUDIO_FMT_OPUS) {
		double q = 0.6;

		(void)sf_command(sf->handle, SFC_SET_VBR_ENCODING_QUALITY,
		    &q, sizeof q);
	}

	sf->fd = fd;
	sf->in_channels = nchannels;
	sf->out_channels = out_ch;

	writer->context = sf;
	writer->write = sndfile_write;
	writer->destroy = sndfile_destroy;

	return writer;
}

#include <assert.h>
#include <soundio/soundio.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio/writer-raw.h"
#include "audio/writer.h"

struct raw {
	FILE *outfile;
};

static void
raw_write(struct audio_writer *writer, enum SoundIoFormat fmt, char *data, int size,
    int bytes_per_frame)
{
	struct raw *raw;
	size_t amt;

	(void)fmt;
	(void)bytes_per_frame;

	assert(writer != NULL);
	assert(data != NULL);

	raw = writer->context;

	amt = fwrite(data, 1, size, raw->outfile);
	if ((int)amt != size) {
		perror("fwrite");
		exit(EXIT_FAILURE);
	}
}

static void
raw_destroy(struct audio_writer *writer)
{
	struct raw *raw;

	assert(writer);
	raw = writer->context;

	free(raw);
	free(writer);
}

struct audio_writer *
audio_writer_raw(FILE *outfile)
{
	struct audio_writer *writer;
	struct raw *raw;

	assert(outfile != NULL);

	writer = malloc(sizeof *writer);
	if (!writer) {
		fprintf(stderr, "No memory for audio writer\n");
	}

	raw = malloc(sizeof *raw);
	if (!raw) {
		fprintf(stderr, "No memory for raw audio writer\n");
		exit(EXIT_FAILURE);
	}

	raw->outfile = outfile;

	writer->context = raw;
	writer->write = raw_write;
	writer->destroy = raw_destroy;

	return writer;
}

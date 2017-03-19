#ifndef AUDIO_WRITER_H
#define AUDIO_WRITER_H

#include <stdio.h>
#include <soundio/soundio.h>

struct audio_writer {
	void *context;
	void (*write)(struct audio_writer *writer, enum SoundIoFormat fmt,
		      char *data, int size, int bytes_per_frame);
	void (*destroy)(struct audio_writer *writer);
};

static inline void
audio_writer_write(struct audio_writer *writer, enum SoundIoFormat fmt,
		   char *data, int size, int bytes_per_frame)
{
	writer->write(writer, fmt, data, size, bytes_per_frame);
}

static inline void
audio_writer_destroy(struct audio_writer *writer)
{
	writer->destroy(writer);
}

#endif /* AUDIO_WRITER_H */

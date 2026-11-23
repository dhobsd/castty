#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <soundio/soundio.h>

#include "audio.h"
#include "audio/writer.h"
#include "audio/writer-lame.h"
#include "audio/writer-raw.h"
#include "json.h"

#ifdef WITH_LAME
static void
put_u16le(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void
put_u16be(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

static void
put_u32le(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static void
put_u32be(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static void
put_u64le(unsigned char *p, uint64_t v)
{
	put_u32le(p, (uint32_t)v);
	put_u32le(p + 4, (uint32_t)(v >> 32));
}

static void
put_u64be(unsigned char *p, uint64_t v)
{
	put_u32be(p, (uint32_t)(v >> 32));
	put_u32be(p + 4, (uint32_t)v);
}
#endif

static void
test_json_escape(void)
{
	char *out;

	out = json_escape("hello world");
	assert(strcmp(out, "hello world") == 0);
	free(out);

	out = json_escape("a\"b\\c\n\r\t\b\f\x01\x1f");
	assert(strcmp(out, "a\\\"b\\\\c\\n\\r\\t\\b\\f\\u0001\\u001f") == 0);
	free(out);

	out = json_escape("caf\xc3\xa9");
	assert(strcmp(out, "caf\xc3\xa9") == 0);
	free(out);
}

static void
test_audio_paused_lifecycle(void)
{
	char path[] = "/tmp/castty_unit_audio_XXXXXX";
	int fd;

	assert(audio_clock_ms() == 0.0);

	fd = mkstemp(path);
	assert(fd >= 0);
	close(fd);

	audio_init("nonexistent-device", path, 0);
	assert(audio_clock_ms() == 0.0);
	audio_toggle_mute();
	audio_stop();
	assert(audio_clock_ms() == 0.0);
	audio_exit();

	assert(audio_clock_ms() == 0.0);
	unlink(path);
}

static void
test_writer_raw(void)
{
	FILE *fp;
	struct audio_writer *aw;
	unsigned char payload[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	unsigned char got[sizeof payload];
	long fsize;

	fp = tmpfile();
	assert(fp != NULL);

	aw = audio_writer_raw(fp);
	assert(aw != NULL);
	audio_writer_write(aw, SoundIoFormatS16LE, (char *)payload, 0, 4);
	audio_writer_write(aw, SoundIoFormatS16LE, (char *)payload, sizeof payload, 4);
	audio_writer_destroy(aw);

	fflush(fp);
	assert(fseek(fp, 0, SEEK_END) == 0);
	fsize = ftell(fp);
	assert(fsize == (long)sizeof payload);

	rewind(fp);
	assert(fread(got, 1, sizeof got, fp) == sizeof got);
	assert(memcmp(got, payload, sizeof payload) == 0);
	fclose(fp);
}

#ifdef WITH_LAME
struct lame_fmt_case {
	enum SoundIoFormat fmt;
	int sample_bytes;
	const char *name;
};

static const struct lame_fmt_case lame_fmt_cases[] = {
	{ SoundIoFormatS8, 1, "S8" },
	{ SoundIoFormatU8, 1, "U8" },
	{ SoundIoFormatS16LE, 2, "S16LE" },
	{ SoundIoFormatS16BE, 2, "S16BE" },
	{ SoundIoFormatU16LE, 2, "U16LE" },
	{ SoundIoFormatU16BE, 2, "U16BE" },
	{ SoundIoFormatS24LE, 4, "S24LE" },
	{ SoundIoFormatS24BE, 4, "S24BE" },
	{ SoundIoFormatU24LE, 4, "U24LE" },
	{ SoundIoFormatU24BE, 4, "U24BE" },
	{ SoundIoFormatS32LE, 4, "S32LE" },
	{ SoundIoFormatS32BE, 4, "S32BE" },
	{ SoundIoFormatU32LE, 4, "U32LE" },
	{ SoundIoFormatU32BE, 4, "U32BE" },
	{ SoundIoFormatFloat32LE, 4, "F32LE" },
	{ SoundIoFormatFloat32BE, 4, "F32BE" },
	{ SoundIoFormatFloat64LE, 8, "F64LE" },
	{ SoundIoFormatFloat64BE, 8, "F64BE" },
};

static void
encode_tone_sample(enum SoundIoFormat fmt, unsigned char *p, double s)
{
	switch (fmt) {
	case SoundIoFormatS8:
		p[0] = (unsigned char)(int8_t)lrint(s * 127.0);
		break;
	case SoundIoFormatU8:
		p[0] = (unsigned char)lrint((s + 1.0) * 127.5);
		break;
	case SoundIoFormatS16LE:
		put_u16le(p, (uint16_t)(int16_t)lrint(s * 32767.0));
		break;
	case SoundIoFormatS16BE:
		put_u16be(p, (uint16_t)(int16_t)lrint(s * 32767.0));
		break;
	case SoundIoFormatU16LE:
		put_u16le(p, (uint16_t)lrint(s * 32767.0 + 32768.0));
		break;
	case SoundIoFormatU16BE:
		put_u16be(p, (uint16_t)lrint(s * 32767.0 + 32768.0));
		break;
	case SoundIoFormatS24LE:
		put_u32le(p, ((uint32_t)(int32_t)lrint(s * 8388607.0)) & 0xffffffu);
		break;
	case SoundIoFormatS24BE:
		put_u32be(p, ((uint32_t)(int32_t)lrint(s * 8388607.0)) & 0xffffffu);
		break;
	case SoundIoFormatU24LE:
		put_u32le(p, (uint32_t)lrint(s * 8388607.0 + 8388608.0));
		break;
	case SoundIoFormatU24BE:
		put_u32be(p, (uint32_t)lrint(s * 8388607.0 + 8388608.0));
		break;
	case SoundIoFormatS32LE:
		put_u32le(p, (uint32_t)(int32_t)llround(s * 2147483647.0));
		break;
	case SoundIoFormatS32BE:
		put_u32be(p, (uint32_t)(int32_t)llround(s * 2147483647.0));
		break;
	case SoundIoFormatU32LE:
		put_u32le(p, (uint32_t)llround(s * 2147483647.0 + 2147483648.0));
		break;
	case SoundIoFormatU32BE:
		put_u32be(p, (uint32_t)llround(s * 2147483647.0 + 2147483648.0));
		break;
	case SoundIoFormatFloat32LE: {
		float fs = (float)s;
		uint32_t u;

		memcpy(&u, &fs, sizeof u);
		put_u32le(p, u);
		break;
	}
	case SoundIoFormatFloat32BE: {
		float fs = (float)s;
		uint32_t u;

		memcpy(&u, &fs, sizeof u);
		put_u32be(p, u);
		break;
	}
	case SoundIoFormatFloat64LE: {
		uint64_t u;

		memcpy(&u, &s, sizeof u);
		put_u64le(p, u);
		break;
	}
	case SoundIoFormatFloat64BE: {
		uint64_t u;

		memcpy(&u, &s, sizeof u);
		put_u64be(p, u);
		break;
	}
	default:
		abort();
	}
}

static void
write_lame_tone(FILE *fp, enum SoundIoFormat fmt, int sb, int nchannels, int mono,
    int buf_time_s, int frames)
{
	struct audio_writer *aw;
	unsigned char *buf;
	int frame_bytes = nchannels * sb;
	int i, ch;

	aw = audio_writer_lame(fp, 48000, nchannels, buf_time_s, mono);
	assert(aw != NULL);

	buf = calloc((size_t)frames, (size_t)frame_bytes);
	assert(buf != NULL);

	for (i = 0; i < frames; i++) {
		double s = 0.35 * sin(2.0 * 3.14159265358979323846 * 440.0 * i / 48000.0);

		for (ch = 0; ch < nchannels; ch++) {
			encode_tone_sample(fmt,
			    buf + (size_t)i * frame_bytes + (size_t)ch * sb, s);
		}
	}

	/* Verify zero-byte, tiny-frame (blen==0), and full-buffer writes */
	audio_writer_write(aw, fmt, (char *)buf, 0, frame_bytes);
	audio_writer_write(aw, fmt, (char *)buf, frame_bytes, frame_bytes);
	audio_writer_write(aw, fmt, (char *)buf + frame_bytes,
	    (frames - 1) * frame_bytes, frame_bytes);
	audio_writer_destroy(aw);
	free(buf);
}

static void
test_writer_lame(void)
{
	size_t i;

	for (i = 0; i < sizeof lame_fmt_cases / sizeof lame_fmt_cases[0]; i++) {
		FILE *fp = tmpfile();
		long fsize;

		assert(fp != NULL);
		write_lame_tone(fp, lame_fmt_cases[i].fmt,
		    lame_fmt_cases[i].sample_bytes, 2, 0, 2, 24000);
		fflush(fp);
		assert(fseek(fp, 0, SEEK_END) == 0);
		fsize = ftell(fp);
		assert(fsize > 1000);
		fclose(fp);
	}

	/* Verify mono mode and chunked encoding when size exceeds frame_cap */
	{
		FILE *fp = tmpfile();
		long fsize;

		assert(fp != NULL);
		write_lame_tone(fp, SoundIoFormatFloat32LE, 4, 2, 1, 1, 60000);
		fflush(fp);
		assert(fseek(fp, 0, SEEK_END) == 0);
		fsize = ftell(fp);
		assert(fsize > 2000);
		fclose(fp);
	}
	{
		FILE *fp = tmpfile();
		long fsize;

		assert(fp != NULL);
		write_lame_tone(fp, SoundIoFormatS16LE, 2, 1, 1, 1, 16000);
		fflush(fp);
		assert(fseek(fp, 0, SEEK_END) == 0);
		fsize = ftell(fp);
		assert(fsize > 1000);
		fclose(fp);
	}
}

static int
dump_lame_formats(const char *dir)
{
	size_t i;

	for (i = 0; i < sizeof lame_fmt_cases / sizeof lame_fmt_cases[0]; i++) {
		char path[512];
		FILE *fp;
		int n;

		n = snprintf(path, sizeof path, "%s/%s.mp3",
		    dir, lame_fmt_cases[i].name);
		assert(n > 0 && (size_t)n < sizeof path);
		fp = fopen(path, "wb");
		assert(fp != NULL);
		write_lame_tone(fp, lame_fmt_cases[i].fmt,
		    lame_fmt_cases[i].sample_bytes, 2, 0, 2, 48000);
		fclose(fp);
	}
	return 0;
}
#endif

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--dump-lame-formats") == 0) {
#ifdef WITH_LAME
		return dump_lame_formats(argv[2]);
#else
		(void)argv;
		return 77;
#endif
	}

	test_json_escape();
	test_audio_paused_lifecycle();
	test_writer_raw();
#ifdef WITH_LAME
	test_writer_lame();
#endif

	printf("C unit tests passed (%s)\n",
#ifdef WITH_LAME
	    "WITH_LAME=yes"
#else
	    "WITH_LAME=no"
#endif
	);
	return 0;
}

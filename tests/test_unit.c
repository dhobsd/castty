#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sndfile.h>
#include <soundio/soundio.h>

#include "audio.h"
#include "audio/pcm.h"
#include "audio/writer.h"
#include "audio/writer-lame.h"
#include "audio/writer-raw.h"
#include "audio/writer-sndfile.h"
#include "cast.h"
#include "json.h"

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

static void
test_pcm_roundtrip_and_fade(void)
{
	enum SoundIoFormat fmts[] = {
		SoundIoFormatS8, SoundIoFormatU8,
		SoundIoFormatS16LE, SoundIoFormatS16BE,
		SoundIoFormatU16LE, SoundIoFormatU16BE,
		SoundIoFormatS24LE, SoundIoFormatS24BE,
		SoundIoFormatU24LE, SoundIoFormatU24BE,
		SoundIoFormatS32LE, SoundIoFormatS32BE,
		SoundIoFormatU32LE, SoundIoFormatU32BE,
		SoundIoFormatFloat32LE, SoundIoFormatFloat32BE,
		SoundIoFormatFloat64LE, SoundIoFormatFloat64BE,
	};
	unsigned char buf[16];
	size_t i;

	for (i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
		float in = 12345.0f, out;

		pcm_encode_sample(fmts[i], buf, in);
		out = pcm_decode_sample(fmts[i], buf);
		assert(fabsf(out - in) < 300.0f);

		/* Verify positive clamp avoids signed wraparound */
		pcm_encode_sample(fmts[i], buf, 40000.0f);
		out = pcm_decode_sample(fmts[i], buf);
		assert(out > 30000.0f);
	}

	/* Verify fade-out reaches 0 at tail and fade-in starts at 0 */
	{
		float frames[8 * 2];
		int f;

		for (f = 0; f < 8; f++) {
			frames[f * 2] = 0.5f;
			frames[f * 2 + 1] = -0.5f;
		}
		pcm_fade_frames(SoundIoFormatFloat32NE, (unsigned char *)frames,
		    8, 2, 8, 4, 0, 8, 1);
		assert(fabsf(frames[0]) > 0.40f);
		assert(fabsf(frames[7 * 2]) < 1e-6f);
		assert(fabsf(frames[7 * 2 + 1]) < 1e-6f);

		for (f = 0; f < 8; f++) {
			frames[f * 2] = 0.5f;
			frames[f * 2 + 1] = -0.5f;
		}
		pcm_fade_frames(SoundIoFormatFloat32NE, (unsigned char *)frames,
		    8, 2, 8, 4, 0, 8, 0);
		assert(fabsf(frames[0]) < 1e-6f);
		assert(fabsf(frames[1]) < 1e-6f);
		assert(fabsf(frames[7 * 2]) > 0.45f);
	}
}

static void
write_str_file(const char *path, const char *s)
{
	FILE *fp = fopen(path, "wb");

	assert(fp != NULL);
	assert(fwrite(s, 1, strlen(s), fp) == strlen(s));
	fclose(fp);
}

static void
test_cast_inspect(void)
{
	char path[] = "/tmp/castty_unit_cast_XXXXXX";
	char errbuf[256];
	struct cast_append_info info;
	int fd;

	assert(cast_inspect("/tmp/castty_definitely_missing_file_xyz", 0, &info,
	    errbuf, sizeof errbuf) == 0);
	assert(info.exists == 0);

	fd = mkstemp(path);
	assert(fd >= 0);
	close(fd);

	/* v3 with embedded brackets/escapes, resize event, no trailing newline */
	write_str_file(path,
	    "{\"version\": 3, \"term\": {\"cols\": 80, \"rows\": 24}}\n"
	    "[0.5000,\"o\",\"hi \\\"[0]\\\"\"]\n"
	    "[0.2500,\"r\",\"110x33\"]\n"
	    "[0.2500,\"x\",\"0\"]");
	assert(cast_inspect(path, 0, &info, errbuf, sizeof errbuf) == 0);
	assert(info.exists == 1);
	assert(info.version == 3);
	assert(info.cols == 110 && info.rows == 33);
	assert(fabs(info.dur_ms - 1000.0) < 0.01);
	assert(info.need_newline == 1);
	assert(cast_inspect(path, 2, &info, errbuf, sizeof errbuf) != 0);
	cast_append_info_free(&info);

	/* v2 cumulative timestamps + resize */
	write_str_file(path,
	    "{\"version\": 2, \"width\": 80, \"height\": 24}\n"
	    "[0.4000,\"o\",\"a\"]\n"
	    "[1.7500,\"r\",\"100x28\"]\n");
	assert(cast_inspect(path, 0, &info, errbuf, sizeof errbuf) == 0);
	assert(info.version == 2);
	assert(info.cols == 100 && info.rows == 28);
	assert(fabs(info.dur_ms - 1750.0) < 0.01);
	assert(info.need_newline == 0);
	cast_append_info_free(&info);

	/* v1 with duration before stdout */
	write_str_file(path,
	    "{\"version\": 1, \"duration\": 1.5, \"width\": 90, \"height\": 30, "
	    "\"stdout\": [[0.25, \"x\"], [0.5, \"y\"]]}\n");
	assert(cast_inspect(path, 1, &info, errbuf, sizeof errbuf) == 0);
	assert(info.version == 1);
	assert(info.cols == 90 && info.rows == 30);
	assert(fabs(info.v1_events_ms - 750.0) < 0.01);
	assert(fabs(info.dur_ms - 1500.0) < 0.01);
	assert(info.v1_empty == 0);
	assert(info.v1_rewrite_buf != NULL);
	cast_append_info_free(&info);

	/* Invalid file rejected cleanly */
	write_str_file(path, "not valid json\n");
	assert(cast_inspect(path, 0, &info, errbuf, sizeof errbuf) != 0);

	unlink(path);
}

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
write_writer_tone(struct audio_writer *aw, enum SoundIoFormat fmt, int sb,
    int nchannels, int sample_rate, int frames)
{
	unsigned char *buf;
	int frame_bytes = nchannels * sb;
	int i, ch;

	assert(aw != NULL);

	buf = calloc((size_t)frames, (size_t)frame_bytes);
	assert(buf != NULL);

	for (i = 0; i < frames; i++) {
		double s = 0.35 *
		    sin(2.0 * 3.14159265358979323846 * 440.0 * i / (double)sample_rate);

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
write_lame_tone(FILE *fp, enum SoundIoFormat fmt, int sb, int nchannels, int mono,
    int buf_time_s, int frames)
{
	struct audio_writer *aw = audio_writer_lame(fp, 48000, nchannels,
	    buf_time_s, mono);

	write_writer_tone(aw, fmt, sb, nchannels, 48000, frames);
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

static void
test_writer_sndfile(void)
{
	static const struct {
		enum audio_format afmt;
		int exp_major;
		int exp_sub;
		int lossless;
		int mono;
	} cases[] = {
		{ AUDIO_FMT_WAV, SF_FORMAT_WAV, SF_FORMAT_PCM_16, 1, 0 },
		{ AUDIO_FMT_WAV, SF_FORMAT_WAV, SF_FORMAT_PCM_16, 1, 1 },
		{ AUDIO_FMT_FLAC, SF_FORMAT_FLAC, SF_FORMAT_PCM_16, 1, 0 },
		{ AUDIO_FMT_FLAC, SF_FORMAT_FLAC, SF_FORMAT_PCM_16, 1, 1 },
		{ AUDIO_FMT_OPUS, SF_FORMAT_OGG, SF_FORMAT_OPUS, 0, 0 },
		{ AUDIO_FMT_OPUS, SF_FORMAT_OGG, SF_FORMAT_OPUS, 0, 1 },
		{ AUDIO_FMT_OGG, SF_FORMAT_OGG, SF_FORMAT_VORBIS, 0, 0 },
		{ AUDIO_FMT_OGG, SF_FORMAT_OGG, SF_FORMAT_VORBIS, 0, 1 },
	};
	size_t i;

	assert(audio_set_format("wav") == 0);
	assert(audio_set_format("FLAC") == 0);
	assert(audio_set_format("opus") == 0);
	assert(audio_set_format("ogg") == 0);
	assert(audio_set_format("bogus") == -1);
	assert(audio_set_format("auto") == 0);

	/* Opus rejects unsupported sample rates like 44100 Hz cleanly */
	{
		FILE *fp = tmpfile();

		assert(fp != NULL);
		assert(audio_writer_sndfile(fp, AUDIO_FMT_OPUS, 44100, 2, 1,
		    0) == NULL);
		fclose(fp);
	}

	for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		char path[] = "/tmp/castty_sf_XXXXXX";
		int fd = mkstemp(path);
		FILE *fp;
		struct audio_writer *aw;
		SF_INFO info;
		SNDFILE *sf;
		float fbuf[24000 * 2];
		sf_count_t nread;
		double sq = 0.0;
		int out_ch = cases[i].mono ? 1 : 2;
		int k;

		assert(fd >= 0);
		fp = fdopen(fd, "w+b");
		assert(fp != NULL);

		aw = audio_writer_sndfile(fp, cases[i].afmt, 48000, 2, 1,
		    cases[i].mono);
		write_writer_tone(aw, SoundIoFormatFloat32LE, 4, 2, 48000,
		    24000);
		fclose(fp);

		memset(&info, 0, sizeof info);
		sf = sf_open(path, SFM_READ, &info);
		assert(sf != NULL);
		assert((info.format & SF_FORMAT_TYPEMASK) == cases[i].exp_major);
		assert((info.format & SF_FORMAT_SUBMASK) == cases[i].exp_sub);
		assert(info.samplerate == 48000);
		assert(info.channels == out_ch);
		assert(info.frames > 0);
		if (cases[i].lossless) {
			assert(info.frames == 24000);
		}

		nread = sf_readf_float(sf, fbuf, 24000);
		assert(nread > 20000);
		for (k = 0; k < (int)nread * out_ch; k++) {
			sq += (double)fbuf[k] * (double)fbuf[k];
		}
		assert(sqrt(sq / (double)(nread * out_ch)) > 0.20);

		if (cases[i].lossless) {
			for (k = 0; k < 24000; k++) {
				double exp_v = 0.35 *
				    sin(2.0 * 3.14159265358979323846 * 440.0 * k / 48000.0);

				assert(fabs((double)fbuf[k * out_ch] - exp_v) < 1e-4);
			}
		}

		sf_close(sf);
		unlink(path);
	}
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--dump-lame-formats") == 0) {
		return dump_lame_formats(argv[2]);
	}

	test_json_escape();
	test_audio_paused_lifecycle();
	test_writer_raw();
	test_pcm_roundtrip_and_fade();
	test_cast_inspect();
	test_writer_lame();
	test_writer_sndfile();

	printf("C unit tests passed\n");
	return 0;
}

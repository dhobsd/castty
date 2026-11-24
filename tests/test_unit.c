#include <assert.h>
#include <fcntl.h>
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
#include "audio/pcm_track.h"
#include "audio/writer.h"
#include "audio/writer-lame.h"
#include "audio/writer-m4a.h"
#include "audio/writer-raw.h"
#include "audio/writer-sndfile.h"
#include "cast.h"
#include "cast_doc.h"
#include "cast_player.h"
#include "json.h"
#include "tui/hud.h"
#include "tui/screen.h"

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
		    sin(2.0 * M_PI * 440.0 * i / (double)sample_rate);

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
				    sin(2.0 * M_PI * 440.0 * k / 48000.0);

				assert(fabs((double)fbuf[k * out_ch] - exp_v) < 1e-4);
			}
		}

		sf_close(sf);
		unlink(path);
	}
}

struct m4a_test_state {
	int frames;
	int rate;
	double sq;
};

static void
m4a_test_cb(void *userdata, int src_rate, float sl, float sr)
{
	struct m4a_test_state *st = userdata;

	st->rate = src_rate;
	st->sq += (double)sl * (double)sl + (double)sr * (double)sr;
	st->frames++;
}

static void
test_writer_m4a(void)
{
	char bad_path[] = "/tmp/castty_bad_m4a_XXXXXX";
	char errbuf[256];
	int bad_fd, mono;

	assert(audio_set_format("m4a") == 0);
	assert(audio_set_format("M4A") == 0);
	assert(audio_set_format("auto") == 0);

	bad_fd = mkstemp(bad_path);
	assert(bad_fd >= 0);
	assert(write(bad_fd, "not an m4a\n", 11) == 11);
	close(bad_fd);
	assert(audio_m4a_probe(bad_path, NULL, NULL, errbuf,
	    sizeof errbuf) != 0);
	unlink(bad_path);

	for (mono = 0; mono <= 1; mono++) {
		char path[] = "/tmp/castty_m4a_XXXXXX";
		int fd = mkstemp(path);
		FILE *fp;
		struct audio_writer *aw;
		struct m4a_test_state st;
		int rate = 0, stereo = -1;

		assert(fd >= 0);
		fp = fdopen(fd, "w+b");
		assert(fp != NULL);

		aw = audio_writer_m4a(fp, 48000, 2, mono);
		write_writer_tone(aw, SoundIoFormatFloat32LE, 4, 2, 48000,
		    24000);
		fclose(fp);

		assert(audio_m4a_probe(path, &rate, &stereo, errbuf,
		    sizeof errbuf) == 0);
		assert(rate == 48000);
		assert(stereo == !mono);

		memset(&st, 0, sizeof st);
		assert(audio_m4a_decode(path, m4a_test_cb, &st, errbuf,
		    sizeof errbuf) == 0);
		assert(st.rate == 48000);
		assert(st.frames > 20000);
		assert(sqrt(st.sq / (double)(st.frames * 2)) > 0.20);

		unlink(path);
	}
}

static void
read_pipe_avail(int rfd, char *buf, size_t sz, size_t *out_len)
{
	size_t total = 0;

	for (;;) {
		ssize_t n = read(rfd, buf + total, sz - 1 - total);

		if (n <= 0) {
			break;
		}
		total += (size_t)n;
		if (total >= sz - 1) {
			break;
		}
	}
	buf[total] = '\0';
	*out_len = total;
}

static void
test_hud_layout_and_compositor(void)
{
	enum hud_mode mode = HUD_AUTO;
	struct hud_layout lay;
	struct hud_state st;
	struct tui_vterm *tv;
	struct tui_surface *sf;
	struct audio_vu vu;
	int pfd[2];
	char out[16384];
	size_t nread = 0;

	assert(hud_parse_mode("auto", &mode) == 0 && mode == HUD_AUTO);
	assert(hud_parse_mode("SIDE", &mode) == 0 && mode == HUD_SIDE);
	assert(hud_parse_mode("bottom", &mode) == 0 && mode == HUD_BOTTOM);
	assert(hud_parse_mode("off", &mode) == 0 && mode == HUD_OFF);
	assert(hud_parse_mode("none", &mode) == 0 && mode == HUD_OFF);
	assert(hud_parse_mode("nope", &mode) == -1);

	/* Auto at 40x120 reserves 8 side columns cleanly */
	hud_compute_layout(HUD_AUTO, 40, 120, 0, 0, 0, &lay);
	assert(lay.active_mode == HUD_SIDE);
	assert(lay.child_rows == 40 && lay.child_cols == 112);
	assert(lay.hud_col == 112 && lay.hud_cols == HUD_SIDE_COLS);

	/* Explicit -c 90 on 120 cols keeps 90 intact alongside side HUD */
	hud_compute_layout(HUD_AUTO, 40, 120, 0, 90, 0, &lay);
	assert(lay.active_mode == HUD_SIDE);
	assert(lay.child_cols == 90);
	assert(lay.hud_col == 112);

	/* Narrow 24x80 falls back to 1-row bottom bar in auto */
	hud_compute_layout(HUD_AUTO, 24, 80, 0, 0, 0, &lay);
	assert(lay.active_mode == HUD_BOTTOM);
	assert(lay.child_rows == 23 && lay.child_cols == 80);
	assert(lay.hud_row == 23 && lay.hud_cols == 80);

	/* Forced side on 24x80 takes 8 columns */
	hud_compute_layout(HUD_SIDE, 24, 80, 0, 0, 0, &lay);
	assert(lay.active_mode == HUD_SIDE);
	assert(lay.child_rows == 24 && lay.child_cols == 72);

	/* Hidden / off gives full outer dimensions */
	hud_compute_layout(HUD_SIDE, 24, 80, 0, 0, 1, &lay);
	assert(lay.active_mode == HUD_OFF);
	assert(lay.child_rows == 24 && lay.child_cols == 80);

	/* Compositor render + diff + zero-diff idle frame verification */
	assert(pipe(pfd) == 0);
	assert(fcntl(pfd[0], F_SETFL, O_NONBLOCK) == 0);

	hud_compute_layout(HUD_SIDE, 14, 40, 0, 0, 0, &lay);
	tv = tui_vterm_create(lay.child_rows, lay.child_cols, NULL, NULL);
	sf = tui_surface_create(lay.outer_rows, lay.outer_cols, pfd[1]);
	hud_init(&st, 0, 0, 1, "m4a", 0, 0.0);

	tui_vterm_feed(tv, "hello\x1b[32m world\x1b[0m", 21, -1);
	hud_note_output(&st);
	hud_note_marker(&st, 1500.0);

	memset(&vu, 0, sizeof vu);
	vu.active = 1;
	vu.stereo = 1;
	vu.rms_l = 0.25f;
	vu.rms_r = 0.25f;
	vu.peak_l = 0.25f;
	vu.peak_r = 0.25f;
	hud_tick(&st, 1500.0, &vu);

	tui_surface_begin(sf);
	tui_vterm_blit(tv, sf, 0, 0);
	hud_render(&st, &lay, sf);
	tui_surface_flush(sf, 0, 11, 1, 0);

	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(nread > 0);
	assert(strstr(out, "hello") != NULL);
	assert(strstr(out, "REC") != NULL);
	assert(strstr(out, "M4A") != NULL);
	assert(strstr(out, "00:01") != NULL);
	assert(strstr(out, "dB") != NULL);

	/* Identical second frame must emit 0 diff bytes */
	tui_surface_begin(sf);
	tui_vterm_blit(tv, sf, 0, 0);
	hud_render(&st, &lay, sf);
	tui_surface_flush(sf, 0, 11, 1, 0);
	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(nread == 0);

	/* Clipping + mute state renders CLIP/MUTE badges */
	vu.clipped = 1;
	vu.peak_l = 0.995f;
	hud_tick(&st, 2000.0, &vu);
	tui_surface_begin(sf);
	tui_vterm_blit(tv, sf, 0, 0);
	hud_render(&st, &lay, sf);
	tui_surface_flush(sf, 0, 11, 1, 0);
	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(strstr(out, "CLIP") != NULL);

	vu.clipped = 0;
	vu.muted = 1;
	hud_tick(&st, 2500.0, &vu);
	tui_surface_begin(sf);
	tui_vterm_blit(tv, sf, 0, 0);
	hud_render(&st, &lay, sf);
	tui_surface_flush(sf, 0, 11, 1, 0);
	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(strstr(out, "MUTE") != NULL);

	/* Named chapter prompt renders non-destructively and restores child row */
	tui_vterm_feed(tv, "\r\n\x1b[14;1HBOTTOM_CHILD", 21, -1);
	(void)tui_vterm_take_dirty(tv);
	hud_set_prompt(&st, 1, "Intro Setup");
	tui_surface_begin(sf);
	tui_vterm_blit(tv, sf, 0, 0);
	hud_render(&st, &lay, sf);
	tui_surface_flush(sf, 0, 0, 1, 0);
	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(strstr(out, "Chapter: Intro Setup_") != NULL);

	hud_set_prompt(&st, 0, NULL);
	hud_note_marker_label(&st, 2600.0, "Intro Setup");
	tui_surface_begin(sf);
	tui_vterm_blit(tv, sf, 0, 0);
	hud_render(&st, &lay, sf);
	tui_surface_flush(sf, 0, 0, 1, 0);
	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(strstr(out, "BOTTOM_CHILD") != NULL);

	/* Bottom HUD displays named marker flash */
	tui_surface_resize(sf, 24, 80);
	hud_compute_layout(HUD_BOTTOM, 24, 80, 0, 0, 0, &lay);
	tui_surface_invalidate(sf);
	tui_surface_begin(sf);
	tui_vterm_blit(tv, sf, 0, 0);
	hud_render(&st, &lay, sf);
	tui_surface_flush(sf, 0, 0, 1, 0);
	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(strstr(out, "MK:02(Intro Setup)") != NULL);

	tui_vterm_destroy(tv);
	tui_surface_destroy(sf);
	close(pfd[0]);
	close(pfd[1]);
}

static void
write_text_file(const char *path, const char *text)
{
	FILE *fp = fopen(path, "wb");
	size_t len = strlen(text);

	assert(fp != NULL);
	assert(fwrite(text, 1, len, fp) == len);
	assert(fclose(fp) == 0);
}

static void
test_cast_doc(void)
{
	char tmpl1[] = "/tmp/castty_doc_v1_XXXXXX";
	char tmpl2[] = "/tmp/castty_doc_v2_XXXXXX";
	char tmpl3[] = "/tmp/castty_doc_v3_XXXXXX";
	char savep[] = "/tmp/castty_doc_save_XXXXXX";
	char err[256];
	int fd;
	struct cast_doc doc, clone, reloaded;
	struct cast_cut_gap *gaps = NULL;
	size_t ngaps = 0, idx = 0, nred = 0;
	double saved_ms = 0.0;

	fd = mkstemp(tmpl1);
	assert(fd >= 0);
	close(fd);
	fd = mkstemp(tmpl2);
	assert(fd >= 0);
	close(fd);
	fd = mkstemp(tmpl3);
	assert(fd >= 0);
	close(fd);
	fd = mkstemp(savep);
	assert(fd >= 0);
	close(fd);

	/* 1. v1 JSON with unicode escapes & surrogate pair */
	write_text_file(tmpl1,
	    "{\n"
	    "  \"version\": 1,\n"
	    "  \"width\": 80,\n"
	    "  \"height\": 24,\n"
	    "  \"duration\": 1.5,\n"
	    "  \"command\": \"/bin/sh\",\n"
	    "  \"title\": \"v1 demo\",\n"
	    "  \"stdout\": [\n"
	    "    [0.25, \"\\u001b[32mhi\\u001b[0m\\r\\n\"],\n"
	    "    [0.75, \"emoji \\ud83d\\ude00 end\"]\n"
	    "  ]\n"
	    "}\n");
	cast_doc_init(&doc);
	assert(cast_doc_load(tmpl1, &doc, err, sizeof err) == 0);
	assert(doc.version == 1 && doc.cols == 80 && doc.rows == 24);
	assert(doc.nevents == 2);
	assert(fabs(doc.events[0].t_ms - 250.0) < 1e-3);
	assert(fabs(doc.events[1].t_ms - 1000.0) < 1e-3);
	assert(fabs(doc.duration_ms - 1500.0) < 1e-3);
	assert(doc.title != NULL && strcmp(doc.title, "v1 demo") == 0);
	assert(strstr(doc.events[0].data, "\x1b[32mhi\x1b[0m") != NULL);
	assert(strstr(doc.events[1].data, "\xf0\x9f\x98\x80") != NULL);
	assert(cast_doc_save(&doc, savep, err, sizeof err) == 0);
	cast_doc_init(&reloaded);
	assert(cast_doc_load(savep, &reloaded, err, sizeof err) == 0);
	assert(reloaded.version == 1 && reloaded.nevents == 2);
	assert(fabs(reloaded.duration_ms - 1500.0) < 1e-1);
	cast_doc_free(&reloaded);
	cast_doc_free(&doc);

	/* 2. v3 JSONL with nested term + delta timestamps + # comment */
	write_text_file(tmpl3,
	    "{\"version\": 3, \"term\": {\"cols\": 100, \"rows\": 30, "
	    "\"type\": \"xterm-256color\", \"version\": \"1.0\"}, "
	    "\"timestamp\": 1700000000}\n"
	    "[0.1, \"o\", \"secret=TOKEN123\\r\\n\"]\n"
	    "[2.4, \"o\", \"after idle TOKEN123\\r\\n\"]\n"
	    "[0.5, \"m\", \"chap1\"]\n"
	    "# trailing comment\n");
	cast_doc_init(&doc);
	assert(cast_doc_load(tmpl3, &doc, err, sizeof err) == 0);
	assert(doc.version == 3 && doc.cols == 100 && doc.rows == 30);
	assert(doc.nevents == 3);
	assert(fabs(doc.events[0].t_ms - 100.0) < 1e-3);
	assert(fabs(doc.events[1].t_ms - 2500.0) < 1e-3);
	assert(fabs(doc.events[2].t_ms - 3000.0) < 1e-3);
	assert(cast_doc_save(&doc, savep, err, sizeof err) == 0);
	cast_doc_init(&reloaded);
	assert(cast_doc_load(savep, &reloaded, err, sizeof err) == 0);
	assert(reloaded.version == 3 && reloaded.nevents == 3);
	assert(fabs(reloaded.events[2].t_ms - 3000.0) < 1e-2);
	cast_doc_free(&reloaded);
	cast_doc_free(&doc);

	/* 3. v2 timeline edits: clone, marker, find, pause, cut, idle, redact, nudge */
	write_text_file(tmpl2,
	    "{\"version\": 2, \"width\": 80, \"height\": 24, "
	    "\"timestamp\": 1700000000, \"title\": \"orig\"}\n"
	    "[0.10, \"o\", \"first pwd=s3cr3t\"]\n"
	    "[0.50, \"o\", \"middle drop\"]\n"
	    "[1.50, \"o\", \"final s3cr3t\"]\n"
	    "[3.50, \"x\", \"0\"]\n");
	cast_doc_init(&doc);
	assert(cast_doc_load(tmpl2, &doc, err, sizeof err) == 0);
	assert(doc.version == 2 && doc.nevents == 4);

	cast_doc_init(&clone);
	assert(cast_doc_clone(&doc, &clone) == 0);
	cast_doc_set_title(&clone, "edited");
	assert(strcmp(doc.title, "orig") == 0);
	assert(strcmp(clone.title, "edited") == 0);

	assert(cast_doc_add_marker(&doc, 600.0, "markA") == 0);
	assert(cast_doc_find_marker(&doc, 0.0, 1, &idx) == 0);
	assert(doc.events[idx].type == CAST_EV_MARKER);
	assert(cast_doc_set_marker_label(&doc, idx, "markB") == 0);
	assert(strcmp(doc.events[idx].data, "markB") == 0);
	assert(cast_doc_find_marker(&doc, 1000.0, -1, &idx) == 0);
	assert(strcmp(doc.events[idx].data, "markB") == 0);

	assert(cast_doc_find_text(&doc, "final", 0.0, 1, &idx) == 0);
	assert(fabs(doc.events[idx].t_ms - 1500.0) < 1e-3);
	assert(cast_doc_find_text(&doc, "first", 1400.0, -1, &idx) == 0);
	assert(fabs(doc.events[idx].t_ms - 100.0) < 1e-3);

	/* Pause at 1000ms for +500ms shifts >= 1000ms events */
	assert(cast_doc_insert_pause(&doc, 1000.0, 500.0) == 0);
	assert(fabs(doc.duration_ms - 4000.0) < 1e-3);

	/* Redact secret across outputs */
	nred = cast_doc_redact(&doc, "s3cr3t", "****");
	assert(nred == 2);
	assert(cast_doc_find_text(&doc, "s3cr3t", -1.0, 1, &idx) != 0);

	/* Cut [400ms, 800ms) with injected continuity snapshot */
	assert(cast_doc_cut_range(&doc, 400.0, 800.0, "SNAP", 4, 80, 24) == 0);
	assert(fabs(doc.duration_ms - 3600.0) < 1e-3);
	assert(cast_doc_find_text(&doc, "SNAP", -1.0, 1, &idx) == 0);
	assert(fabs(doc.events[idx].t_ms - 400.0) < 1e-3);
	assert(cast_doc_find_text(&doc, "middle drop", -1.0, 1, &idx) != 0);

	/* Compress idle gaps > 500ms across entire doc */
	assert(cast_doc_compress_idle(&doc, 0.0, doc.duration_ms, 500.0,
	    &gaps, &ngaps, &saved_ms) == 0);
	assert(ngaps >= 1 && saved_ms > 500.0);
	free(gaps);

	/* Positive nudge shifts events right cleanly */
	double prev_t0 = doc.events[0].t_ms;
	cast_doc_nudge(&doc, 120.0);
	assert(fabs(doc.events[0].t_ms - (prev_t0 + 120.0)) < 1e-3);

	cast_doc_free(&clone);
	cast_doc_free(&doc);

	/* 4. Sub-0.1ms v3 delta error-diffusion + invalid UTF-8 sanitization */
	cast_doc_init(&doc);
	doc.version = 3;
	for (int i = 1; i <= 1000; i++) {
		assert(cast_doc_insert_event(&doc, (double)i * 0.04,
		    CAST_EV_OUT, ".", 1, 80, 24) == 0);
	}
	assert(cast_doc_insert_event(&doc, 50.0, CAST_EV_OUT,
	    "bad\xffok", 6, 80, 24) == 0);
	assert(cast_doc_save(&doc, savep, err, sizeof err) == 0);
	cast_doc_init(&reloaded);
	assert(cast_doc_load(savep, &reloaded, err, sizeof err) == 0);
	assert(reloaded.nevents == 1001);
	assert(fabs(reloaded.events[999].t_ms - 40.0) < 0.15);
	assert(strstr(reloaded.events[1000].data, "bad\xef\xbf\xbdok") != NULL);
	cast_doc_free(&reloaded);
	cast_doc_free(&doc);

	/* 5. Cross-event keystroke search (ANSI-stripped) and redaction */
	cast_doc_init(&doc);
	doc.version = 3;
	const char *keys = "ls -lt tok999";
	for (size_t k = 0; keys[k] != '\0'; k++) {
		assert(cast_doc_insert_event(&doc, 100.0 + (double)k * 50.0,
		    CAST_EV_OUT, &keys[k], 1, 80, 24) == 0);
	}
	assert(cast_doc_insert_event(&doc, 900.0, CAST_EV_OUT,
	    "\x1b[01;34msrc_dir\x1b[0m\r\n", 17, 80, 24) == 0);
	assert(cast_doc_find_text(&doc, "ls -lt", 0.0, 1, &idx) == 0);
	assert(fabs(doc.events[idx].t_ms - 350.0) < 1e-3);
	assert(cast_doc_find_text(&doc, "src_dir", 0.0, 1, &idx) == 0);
	assert(fabs(doc.events[idx].t_ms - 900.0) < 1e-3);
	assert(cast_doc_redact(&doc, "tok999", "******") == 1);
	assert(cast_doc_find_text(&doc, "tok999", 0.0, 1, &idx) != 0);
	assert(cast_doc_find_text(&doc, "******", 0.0, 1, &idx) == 0);
	cast_doc_free(&doc);

	unlink(tmpl1);
	unlink(tmpl2);
	unlink(tmpl3);
	unlink(savep);
}

static void
test_cast_player(void)
{
	struct cast_doc doc;
	struct cast_player *pl;
	struct tui_surface *sf;
	char line[64];
	char *snap = NULL;
	size_t snap_len = 0;
	int scols = 0, srows = 0;
	int cur_r = -1, cur_c = -1, cur_vis = 0, cur_shape = 0;
	int pfd[2];
	char out[32768];
	size_t nread = 0;

	cast_doc_init(&doc);
	doc.version = 2;
	doc.cols = 60;
	doc.rows = 16;

	/* Generate 300 events (>256 checkpoint stride) with cursor placement */
	for (int i = 0; i < 300; i++) {
		int len;

		if (i == 10) {
			len = snprintf(line, sizeof line,
			    "\x1b[32mEARLY_%03d\x1b[0m\r\n", i);
		} else if (i == 280) {
			len = snprintf(line, sizeof line,
			    "\x1b[2J\x1b[H\x1b[31mLATE_%03d\x1b[5;10H", i);
		} else {
			len = snprintf(line, sizeof line, "ROW_%03d\r\n", i);
		}
		assert(cast_doc_insert_event(&doc, (double)(i + 1) * 10.0,
		    CAST_EV_OUT, line, (size_t)len, 60, 16) == 0);
		if (i == 150) {
			assert(cast_doc_insert_event(&doc, 1515.0,
			    CAST_EV_RESIZE, NULL, 0, 70, 20) == 0);
		}
	}

	pl = cast_player_create(&doc);
	assert(pl != NULL);
	assert(pipe(pfd) == 0);
	assert(fcntl(pfd[0], F_SETFL, O_NONBLOCK) == 0);
	sf = tui_surface_create(16, 70, pfd[1]);
	assert(sf != NULL);

	/* Seek right after event 280 (past checkpoint 256) and verify cursor + blit */
	assert(cast_player_seek(pl, 2815.0) == 0);
	assert(cast_player_cols(pl) == 70 && cast_player_rows(pl) == 20);
	cast_player_get_cursor(pl, &cur_r, &cur_c, &cur_vis, &cur_shape);
	assert(cur_r == 4 && cur_c == 9);
	tui_surface_begin(sf);
	cast_player_blit(pl, sf, 0, 0, 0, 0, 16, 70);
	tui_surface_flush(sf, cur_r, cur_c, cur_vis, cur_shape);
	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(nread > 0 && strstr(out, "LATE_280") != NULL);

	/* Backward scrub before resize and before event 280 */
	assert(cast_player_seek(pl, 200.0) == 0);
	assert(cast_player_cols(pl) == 60 && cast_player_rows(pl) == 16);
	tui_surface_begin(sf);
	cast_player_blit(pl, sf, 0, 0, 0, 0, 16, 60);
	tui_surface_flush(sf, 0, 0, 1, 0);
	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(nread > 0 && strstr(out, "EARLY_010") != NULL);
	assert(strstr(out, "LATE_280") == NULL);

	/* Snapshot ANSI at t=2815 preserves LATE_280 + colors in fresh doc */
	assert(cast_player_snapshot_ansi(&doc, 2815.0, &snap, &snap_len,
	    &scols, &srows) == 0);
	assert(snap != NULL && snap_len > 0 && scols == 70 && srows == 20);
	assert(strstr(snap, "LATE_280") != NULL);
	free(snap);

	cast_player_destroy(pl);
	cast_doc_free(&doc);

	/* Altscreen (?1049h/?1049l) preservation across >256 events + cut snapshot + pan */
	cast_doc_init(&doc);
	doc.version = 3;
	doc.cols = 60;
	doc.rows = 20;
	assert(cast_doc_insert_event(&doc, 10.0, CAST_EV_OUT,
	    "MAIN_SHELL_PROMPT\r\n\x1b[20;1HBOTTOM_ROW_20", 39, 60, 20) == 0);
	assert(cast_doc_insert_event(&doc, 20.0, CAST_EV_OUT,
	    "\x1b[?1049h\x1b[HALT_VIM_VIEW", 23, 60, 20) == 0);
	for (int i = 0; i < 270; i++) {
		int len = snprintf(line, sizeof line, "\x1b[2;1Hvim_%d", i);
		assert(cast_doc_insert_event(&doc, 30.0 + i * 5.0, CAST_EV_OUT,
		    line, (size_t)len, 60, 20) == 0);
	}
	assert(cast_doc_insert_event(&doc, 1500.0, CAST_EV_OUT,
	    "\x1b[?1049l", 8, 60, 20) == 0);
	assert(cast_doc_insert_event(&doc, 1600.0, CAST_EV_OUT,
	    "\r\nAFTER_VIM", 11, 60, 20) == 0);

	pl = cast_player_create(&doc);
	assert(pl != NULL);
	/* Seek to end then backward right after ?1049l: main screen must survive */
	assert(cast_player_seek(pl, 1650.0) == 0);
	assert(cast_player_seek(pl, 1510.0) == 0);
	tui_surface_begin(sf);
	cast_player_blit(pl, sf, 0, 0, 0, 0, 16, 60);
	tui_surface_flush(sf, 0, 0, 1, 0);
	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(strstr(out, "MAIN_SHELL_PROMPT") != NULL);

	/* Viewport panning (src_row=4 on 20-row cast into 16-row surface) shows row 20 */
	tui_surface_begin(sf);
	cast_player_blit(pl, sf, 4, 0, 0, 0, 16, 60);
	tui_surface_flush(sf, 0, 0, 1, 0);
	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(strstr(out, "BOTTOM_ROW_20") != NULL);
	cast_player_destroy(pl);

	/* Cut [5.0ms, 500.0ms) ending INSIDE altscreen preserves BOTH main & alt */
	snap = NULL;
	snap_len = 0;
	assert(cast_player_snapshot_ansi(&doc, 500.0, &snap, &snap_len,
	    &scols, &srows) == 0);
	assert(cast_doc_cut_range(&doc, 5.0, 500.0, snap, snap_len,
	    scols, srows) == 0);
	free(snap);

	pl = cast_player_create(&doc);
	assert(cast_player_seek(pl, 100.0) == 0); /* still inside vim */
	tui_surface_begin(sf);
	cast_player_blit(pl, sf, 0, 0, 0, 0, 16, 60);
	tui_surface_flush(sf, 0, 0, 1, 0);
	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(strstr(out, "ALT_VIM_VIEW") != NULL);

	assert(cast_player_seek(pl, 1020.0) == 0); /* after ?1049l */
	tui_surface_begin(sf);
	cast_player_blit(pl, sf, 0, 0, 0, 0, 16, 60);
	tui_surface_flush(sf, 0, 0, 1, 0);
	read_pipe_avail(pfd[0], out, sizeof out, &nread);
	assert(strstr(out, "MAIN_SHELL_PROMPT") != NULL);

	tui_surface_destroy(sf);
	close(pfd[0]);
	close(pfd[1]);
	cast_player_destroy(pl);
	cast_doc_free(&doc);
}

static void
test_pcm_track(void)
{
	char wavp[] = "/tmp/castty_tr_in_XXXXXX";
	char out_wav[64];
	char out_opus[64];
	char out_m4a[64];
	char err[256];
	int fd;
	FILE *fp;
	struct audio_writer *w;
	struct pcm_track tr;
	struct pcm_edl_snapshot snap;
	float buf[PCM_TRACK_CHANNELS * 4800];
	float frame_pair[2];
	int rate = 0, stereo = 0;
	int dub1_id = -1, dub2_id = -1;
	FILE *dub1_fp = NULL, *dub2_fp = NULL;

	/* Prepare temp paths */
	fd = mkstemp(wavp);
	assert(fd >= 0);
	close(fd);
	/* Keep extensionless base unique, suffix by PID for format tests */
	snprintf(out_wav, sizeof out_wav, "/tmp/castty_tr_%ld.wav",
	    (long)getpid());
	snprintf(out_opus, sizeof out_opus, "/tmp/castty_tr_%ld.opus",
	    (long)getpid());
	snprintf(out_m4a, sizeof out_m4a, "/tmp/castty_tr_%ld.m4a",
	    (long)getpid());

	/* Empty track check */
	pcm_track_init(&tr);
	pcm_track_create_empty(&tr, 3000.0);
	assert(fabs(pcm_track_duration_ms(&tr) - 3000.0) < 1e-2);
	assert(pcm_track_read_frames(&tr, 4800, buf, 100) == 100);
	assert(fabsf(buf[0]) < 1e-6f);
	pcm_track_destroy(&tr);

	/* Write 2s WAV: first 1s = +0.80f step, second 1s = -0.80f step */
	fp = fopen(wavp, "wb");
	assert(fp != NULL);
	w = audio_open_writer(fp, AUDIO_FMT_WAV, PCM_TRACK_RATE, 0);
	assert(w != NULL);
	for (size_t i = 0; i < sizeof buf / sizeof buf[0]; i++) {
		buf[i] = 0.80f;
	}
	for (int c = 0; c < 10; c++) {
		audio_writer_write(w, SoundIoFormatFloat32NE, (char *)buf,
		    (int)sizeof buf, (int)(2 * sizeof(float)));
	}
	for (size_t i = 0; i < sizeof buf / sizeof buf[0]; i++) {
		buf[i] = -0.80f;
	}
	for (int c = 0; c < 10; c++) {
		audio_writer_write(w, SoundIoFormatFloat32NE, (char *)buf,
		    (int)sizeof buf, (int)(2 * sizeof(float)));
	}
	audio_writer_destroy(w);
	fclose(fp);

	pcm_track_init(&tr);
	assert(pcm_track_load(&tr, wavp, AUDIO_FMT_WAV, 2500.0,
	    err, sizeof err) == 0);
	assert(fabs(pcm_track_duration_ms(&tr) - 2500.0) < 1e-2);
	assert(tr.has_audio == 1);

	/* Peak query reflects signal vs trailing silence */
	pcm_track_ensure_peaks(&tr);
	assert(pcm_track_peak_in_ms(&tr, 100.0, 400.0) > 0.70f);
	assert(pcm_track_peak_in_ms(&tr, 2100.0, 2400.0) < 0.01f);

	/* Save EDL snapshot before edits */
	assert(pcm_edl_snapshot_take(&tr, &snap) == 0);

	/* Cut [600ms, 1400ms) joining +0.80f to -0.80f at frame 28800 */
	assert(pcm_track_cut_range(&tr, 600.0, 1400.0) == 0);
	assert(fabs(pcm_track_duration_ms(&tr) - 1700.0) < 1e-2);

	/* Verify 10 ms half-cosine fade eliminates step pop at cut frame 28800 */
	assert(pcm_track_read_frames(&tr, 28800 - 600, buf, 1200) == 1200);
	assert(buf[0] > 0.70f);
	assert(fabsf(buf[600 * 2]) < 0.05f);
	assert(buf[1199 * 2] < -0.70f);
	for (int f = 0; f < 1199; f++) {
		float diff = fabsf(buf[(f + 1) * 2] - buf[f * 2]);
		assert(diff < 0.02f);
	}

	/* Mute [100ms, 300ms) -> center is digital silence */
	assert(pcm_track_mute_range(&tr, 100.0, 300.0) == 0);
	assert(pcm_track_read_frames(&tr, 200 * 48, buf, 480) == 480);
	assert(fabsf(buf[240 * 2]) < 1e-5f);

	/* Insert 200ms silence at 200ms -> coalesces without growing pieces */
	assert(pcm_track_insert_silence(&tr, 100.0, 100.0) == 0);
	size_t pcount = tr.npieces;
	assert(pcm_track_insert_silence(&tr, 200.0, 50.0) == 0);
	assert(pcm_track_insert_silence(&tr, 250.0, 50.0) == 0);
	assert(tr.npieces == pcount);
	assert(fabs(pcm_track_duration_ms(&tr) - 1900.0) < 1e-2);

	/* Nudge +100ms and -100ms roundtrip */
	assert(pcm_track_nudge(&tr, 100.0) == 0);
	assert(fabs(pcm_track_duration_ms(&tr) - 2000.0) < 1e-2);
	assert(pcm_track_nudge(&tr, -100.0) == 0);
	assert(fabs(pcm_track_duration_ms(&tr) - 1900.0) < 1e-2);

	/* Restore snapshot back to clean 2500ms track */
	assert(pcm_edl_snapshot_restore(&tr, &snap) == 0);
	assert(fabs(pcm_track_duration_ms(&tr) - 2500.0) < 1e-2);
	pcm_edl_snapshot_free(&snap);

	/* Overdub pass 1 (mix + duck=0.25) and pass 2 (bakes pass 1!) */
	assert(pcm_track_new_scratch(&tr, &dub1_id, &dub1_fp) == 0);
	for (int f = 0; f < 24000; f++) {
		frame_pair[0] = 0.30f;
		frame_pair[1] = 0.30f;
		assert(fwrite(frame_pair, sizeof(float), 2, dub1_fp) == 2);
	}
	fflush(dub1_fp);
	assert(pcm_track_apply_recording(&tr, 200.0, dub1_id, 0, 24000,
	    0, 0.25f) == 0);
	/* Pass 1 midpoint around 350ms: 0.80 * 0.25 + 0.30 = 0.50f */
	assert(pcm_track_read_frames(&tr, 350 * 48, buf, 10) == 10);
	assert(fabsf(buf[0] - 0.50f) < 0.02f);

	/* Pass 2 overlapping at 300ms (0.50f * 0.5 + 0.15 = 0.40f) */
	assert(pcm_track_new_scratch(&tr, &dub2_id, &dub2_fp) == 0);
	for (int f = 0; f < 24000; f++) {
		frame_pair[0] = 0.15f;
		frame_pair[1] = 0.15f;
		assert(fwrite(frame_pair, sizeof(float), 2, dub2_fp) == 2);
	}
	fflush(dub2_fp);
	assert(pcm_track_apply_recording(&tr, 300.0, dub2_id, 0, 24000,
	    0, 0.50f) == 0);
	assert(pcm_track_read_frames(&tr, 350 * 48, buf, 10) == 10);
	assert(fabsf(buf[0] - 0.40f) < 0.02f);

	/* Replace mode over [2000ms, 2500ms) extends cleanly past end too */
	assert(pcm_track_apply_recording(&tr, 2200.0, dub1_id, 0, 24000,
	    1, 1.0f) == 0);
	assert(fabs(pcm_track_duration_ms(&tr) - 2700.0) < 1e-2);

	/* Save roundtrips: WAV, Opus, M4A */
	assert(pcm_track_save(&tr, out_wav, AUDIO_FMT_WAV, 0,
	    err, sizeof err) == 0);
	assert(audio_probe_file(out_wav, AUDIO_FMT_WAV, &rate, &stereo,
	    err, sizeof err) == 0);
	assert(rate == PCM_TRACK_RATE && stereo == 1);

	assert(pcm_track_save(&tr, out_opus, AUDIO_FMT_OPUS, 0,
	    err, sizeof err) == 0);
	assert(audio_probe_file(out_opus, AUDIO_FMT_OPUS, &rate, &stereo,
	    err, sizeof err) == 0);

	assert(pcm_track_save(&tr, out_m4a, AUDIO_FMT_M4A, 0,
	    err, sizeof err) == 0);
	assert(audio_probe_file(out_m4a, AUDIO_FMT_M4A, &rate, &stereo,
	    err, sizeof err) == 0);

	pcm_track_destroy(&tr);

	/* Reload saved M4A into a fresh pcm_track */
	pcm_track_init(&tr);
	assert(pcm_track_load(&tr, out_m4a, AUDIO_FMT_M4A, 0.0,
	    err, sizeof err) == 0);
	assert(tr.has_audio == 1 && tr.total_frames > 48000);
	pcm_track_destroy(&tr);

	unlink(wavp);
	unlink(out_wav);
	unlink(out_opus);
	unlink(out_m4a);
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
	test_writer_m4a();
	test_hud_layout_and_compositor();
	test_cast_doc();
	test_cast_player();
	test_pcm_track();

	printf("C unit tests passed\n");
	return 0;
}

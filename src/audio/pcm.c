#include <math.h>
#include <stdint.h>
#include <string.h>

#include "audio/pcm.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

static void
write_u16le(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void
write_u16be(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

static void
write_u32le(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static void
write_u32be(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static void
write_u64le(unsigned char *p, uint64_t v)
{
	write_u32le(p, (uint32_t)v);
	write_u32le(p + 4, (uint32_t)(v >> 32));
}

static void
write_u64be(unsigned char *p, uint64_t v)
{
	write_u32be(p, (uint32_t)(v >> 32));
	write_u32be(p + 4, (uint32_t)v);
}

static double
clamp_range(double v, double lo, double hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

int
pcm_sample_bytes(enum SoundIoFormat fmt)
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

float
pcm_decode_sample(enum SoundIoFormat fmt, const unsigned char *p)
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
	case SoundIoFormatS24LE: {
		uint32_t u = read_u32le(p) & 0xffffffu;
		int32_t s = (int32_t)(u ^ 0x800000u) - 0x800000;

		return (float)s / 256.0f;
	}
	case SoundIoFormatS24BE: {
		uint32_t u = read_u32be(p) & 0xffffffu;
		int32_t s = (int32_t)(u ^ 0x800000u) - 0x800000;

		return (float)s / 256.0f;
	}
	case SoundIoFormatU24LE:
		return ((float)(read_u32le(p) & 0xffffffu) - 8388608.0f) / 256.0f;
	case SoundIoFormatU24BE:
		return ((float)(read_u32be(p) & 0xffffffu) - 8388608.0f) / 256.0f;
	case SoundIoFormatS32LE: {
		uint32_t u = read_u32le(p);
		int32_t s;

		memcpy(&s, &u, sizeof s);
		return (float)((double)s / 65536.0);
	}
	case SoundIoFormatS32BE: {
		uint32_t u = read_u32be(p);
		int32_t s;

		memcpy(&s, &u, sizeof s);
		return (float)((double)s / 65536.0);
	}
	case SoundIoFormatU32LE:
		return (float)(((double)read_u32le(p) - 2147483648.0) / 65536.0);
	case SoundIoFormatU32BE:
		return (float)(((double)read_u32be(p) - 2147483648.0) / 65536.0);
	case SoundIoFormatFloat32LE: {
		uint32_t u = read_u32le(p);
		float f;

		memcpy(&f, &u, sizeof f);
		return f * 32768.0f;
	}
	case SoundIoFormatFloat32BE: {
		uint32_t u = read_u32be(p);
		float f;

		memcpy(&f, &u, sizeof f);
		return f * 32768.0f;
	}
	case SoundIoFormatFloat64LE: {
		uint64_t u = read_u64le(p);
		double d;

		memcpy(&d, &u, sizeof d);
		return (float)(d * 32768.0);
	}
	case SoundIoFormatFloat64BE: {
		uint64_t u = read_u64be(p);
		double d;

		memcpy(&d, &u, sizeof d);
		return (float)(d * 32768.0);
	}
	default:
		return 0.0f;
	}
}

void
pcm_encode_sample(enum SoundIoFormat fmt, unsigned char *p, float val)
{
	switch (fmt) {
	case SoundIoFormatS8: {
		int8_t s = (int8_t)llround(clamp_range((double)val / 256.0, -128.0, 127.0));

		memcpy(p, &s, sizeof s);
		break;
	}
	case SoundIoFormatU8:
		p[0] = (unsigned char)llround(clamp_range((double)val / 256.0 + 128.0, 0.0, 255.0));
		break;
	case SoundIoFormatS16LE: {
		int16_t s = (int16_t)llround(clamp_range(val, -32768.0, 32767.0));
		uint16_t u;

		memcpy(&u, &s, sizeof u);
		write_u16le(p, u);
		break;
	}
	case SoundIoFormatS16BE: {
		int16_t s = (int16_t)llround(clamp_range(val, -32768.0, 32767.0));
		uint16_t u;

		memcpy(&u, &s, sizeof u);
		write_u16be(p, u);
		break;
	}
	case SoundIoFormatU16LE:
		write_u16le(p, (uint16_t)llround(clamp_range((double)val + 32768.0, 0.0, 65535.0)));
		break;
	case SoundIoFormatU16BE:
		write_u16be(p, (uint16_t)llround(clamp_range((double)val + 32768.0, 0.0, 65535.0)));
		break;
	case SoundIoFormatS24LE: {
		int32_t s = (int32_t)llround(clamp_range((double)val * 256.0, -8388608.0, 8388607.0));

		write_u32le(p, ((uint32_t)s) & 0xffffffu);
		break;
	}
	case SoundIoFormatS24BE: {
		int32_t s = (int32_t)llround(clamp_range((double)val * 256.0, -8388608.0, 8388607.0));

		write_u32be(p, ((uint32_t)s) & 0xffffffu);
		break;
	}
	case SoundIoFormatU24LE:
		write_u32le(p, (uint32_t)llround(clamp_range((double)val * 256.0 + 8388608.0,
		    0.0, 16777215.0)));
		break;
	case SoundIoFormatU24BE:
		write_u32be(p, (uint32_t)llround(clamp_range((double)val * 256.0 + 8388608.0,
		    0.0, 16777215.0)));
		break;
	case SoundIoFormatS32LE: {
		int32_t s = (int32_t)llround(clamp_range((double)val * 65536.0,
		    -2147483648.0, 2147483647.0));
		uint32_t u;

		memcpy(&u, &s, sizeof u);
		write_u32le(p, u);
		break;
	}
	case SoundIoFormatS32BE: {
		int32_t s = (int32_t)llround(clamp_range((double)val * 65536.0,
		    -2147483648.0, 2147483647.0));
		uint32_t u;

		memcpy(&u, &s, sizeof u);
		write_u32be(p, u);
		break;
	}
	case SoundIoFormatU32LE:
		write_u32le(p, (uint32_t)llround(clamp_range((double)val * 65536.0 + 2147483648.0,
		    0.0, 4294967295.0)));
		break;
	case SoundIoFormatU32BE:
		write_u32be(p, (uint32_t)llround(clamp_range((double)val * 65536.0 + 2147483648.0,
		    0.0, 4294967295.0)));
		break;
	case SoundIoFormatFloat32LE: {
		float f = (float)((double)val / 32768.0);
		uint32_t u;

		memcpy(&u, &f, sizeof u);
		write_u32le(p, u);
		break;
	}
	case SoundIoFormatFloat32BE: {
		float f = (float)((double)val / 32768.0);
		uint32_t u;

		memcpy(&u, &f, sizeof u);
		write_u32be(p, u);
		break;
	}
	case SoundIoFormatFloat64LE: {
		double d = (double)val / 32768.0;
		uint64_t u;

		memcpy(&u, &d, sizeof u);
		write_u64le(p, u);
		break;
	}
	case SoundIoFormatFloat64BE: {
		double d = (double)val / 32768.0;
		uint64_t u;

		memcpy(&u, &d, sizeof u);
		write_u64be(p, u);
		break;
	}
	default:
		break;
	}
}

void
pcm_fade_frames(enum SoundIoFormat fmt, unsigned char *data, int nframes,
    int nchannels, int frame_bytes, int sample_bytes, int start_pos,
    int total_fade, int fade_out)
{
	int i, ch;

	if (total_fade <= 0 || nframes <= 0) {
		return;
	}

	for (i = 0; i < nframes; i++) {
		int pos = start_pos + i;
		double t, gain;
		unsigned char *frame;

		if (pos >= total_fade) {
			gain = fade_out ? 0.0 : 1.0;
		} else {
			t = fade_out ? (double)(pos + 1) / (double)total_fade :
			    (double)pos / (double)total_fade;
			gain = fade_out ? 0.5 * (1.0 + cos(M_PI * t)) :
			    0.5 * (1.0 - cos(M_PI * t));
		}

		frame = data + (size_t)i * frame_bytes;
		for (ch = 0; ch < nchannels; ch++) {
			unsigned char *smp = frame + (size_t)ch * sample_bytes;
			float v = pcm_decode_sample(fmt, smp);

			pcm_encode_sample(fmt, smp, (float)(v * gain));
		}
	}
}

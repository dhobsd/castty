#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <soundio/soundio.h>

#include "audio/pcm.h"
#include "audio/writer-m4a.h"
#include "audio/writer.h"

enum {
	M4A_AVIO_BUFSZ = 32768,
};

struct m4a_writer {
	FILE *outfile;
	AVFormatContext *fmt_ctx;
	AVCodecContext *codec_ctx;
	AVStream *stream;
	AVFrame *frame;
	AVPacket *pkt;
	int in_channels;
	int out_channels;
	int frame_cap;
	int acc;
	int64_t next_pts;
};

#if LIBAVFORMAT_VERSION_MAJOR >= 61
static int
stdio_write_packet(void *opaque, const uint8_t *buf, int buf_size)
#else
static int
stdio_write_packet(void *opaque, uint8_t *buf, int buf_size)
#endif
{
	FILE *fp = opaque;
	size_t nw;

	if (buf_size <= 0) {
		return 0;
	}
	nw = fwrite(buf, 1, (size_t)buf_size, fp);
	if (nw != (size_t)buf_size) {
		return AVERROR(EIO);
	}
	return (int)nw;
}

static int64_t
stdio_seek(void *opaque, int64_t offset, int whence)
{
	FILE *fp = opaque;
	off_t pos;

	whence &= ~AVSEEK_FORCE;
	if (whence == AVSEEK_SIZE) {
		struct stat st;

		if (fflush(fp) != 0 || fstat(fileno(fp), &st) != 0) {
			return AVERROR(errno);
		}
		return (int64_t)st.st_size;
	}
	if (fseeko(fp, (off_t)offset, whence) != 0) {
		return AVERROR(errno);
	}
	pos = ftello(fp);
	return (pos < 0) ? AVERROR(errno) : (int64_t)pos;
}

static int
encoder_supports_fltp(const AVCodec *codec)
{
	const enum AVSampleFormat *fmts = NULL;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
	if (avcodec_get_supported_config(NULL, codec,
	    AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void **)&fmts, NULL) < 0) {
		return 0;
	}
#else
	fmts = codec->sample_fmts;
#endif
	if (fmts == NULL) {
		return 1;
	}
	for (int i = 0; fmts[i] != AV_SAMPLE_FMT_NONE; i++) {
		if (fmts[i] == AV_SAMPLE_FMT_FLTP) {
			return 1;
		}
	}
	return 0;
}

static void
drain_encoder_packets(struct m4a_writer *m4a)
{
	for (;;) {
		int ret = avcodec_receive_packet(m4a->codec_ctx, m4a->pkt);

		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			break;
		}
		if (ret < 0) {
			char err[AV_ERROR_MAX_STRING_SIZE];

			av_strerror(ret, err, sizeof err);
			fprintf(stderr, "avcodec_receive_packet failed: %s\n",
			    err);
			exit(EXIT_FAILURE);
		}

		av_packet_rescale_ts(m4a->pkt, m4a->codec_ctx->time_base,
		    m4a->stream->time_base);
		m4a->pkt->stream_index = m4a->stream->index;

		ret = av_interleaved_write_frame(m4a->fmt_ctx, m4a->pkt);
		av_packet_unref(m4a->pkt);
		if (ret < 0) {
			char err[AV_ERROR_MAX_STRING_SIZE];

			av_strerror(ret, err, sizeof err);
			fprintf(stderr, "av_interleaved_write_frame failed: %s\n",
			    err);
			exit(EXIT_FAILURE);
		}
	}
}

static void
flush_current_frame(struct m4a_writer *m4a, int nsamples)
{
	int ret;

	m4a->frame->nb_samples = nsamples;
	m4a->frame->pts = m4a->next_pts;
	m4a->next_pts += nsamples;

	ret = avcodec_send_frame(m4a->codec_ctx, m4a->frame);
	if (ret < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE];

		av_strerror(ret, err, sizeof err);
		fprintf(stderr, "avcodec_send_frame failed: %s\n", err);
		exit(EXIT_FAILURE);
	}
	drain_encoder_packets(m4a);
	m4a->acc = 0;
}

static void
m4a_write(struct audio_writer *writer, enum SoundIoFormat fmt, char *data,
    int size, int bytes_per_frame)
{
	struct m4a_writer *m4a;
	const unsigned char *src;
	int nsamples, sample_bytes, offset;

	assert(writer != NULL);
	assert(data != NULL);
	assert(bytes_per_frame > 0);

	if (size <= 0) {
		return;
	}

	m4a = writer->context;
	sample_bytes = pcm_sample_bytes(fmt);
	if (sample_bytes == 0 ||
	    bytes_per_frame != m4a->in_channels * sample_bytes) {
		fprintf(stderr, "Invalid audio format or frame size\n");
		exit(EXIT_FAILURE);
	}

	nsamples = size / bytes_per_frame;
	if (nsamples <= 0) {
		return;
	}

	src = (const unsigned char *)data;
	for (offset = 0; offset < nsamples;) {
		float *ch0, *ch1;
		int take, i;

		if (m4a->acc == 0 && av_frame_make_writable(m4a->frame) < 0) {
			fprintf(stderr, "av_frame_make_writable failed\n");
			exit(EXIT_FAILURE);
		}

		ch0 = (float *)m4a->frame->data[0];
		ch1 = (m4a->out_channels > 1) ? (float *)m4a->frame->data[1] : NULL;
		take = m4a->frame_cap - m4a->acc;
		if (take > nsamples - offset) {
			take = nsamples - offset;
		}

		for (i = 0; i < take; i++) {
			const unsigned char *frame = src +
			    (size_t)bytes_per_frame * (size_t)i;
			float l = pcm_decode_sample(fmt, frame) / 32768.0f;
			float r = (m4a->in_channels > 1) ?
			    (pcm_decode_sample(fmt, frame + sample_bytes) / 32768.0f) : l;

			if (m4a->out_channels == 1) {
				ch0[m4a->acc + i] = 0.5f * (l + r);
			} else {
				ch0[m4a->acc + i] = l;
				ch1[m4a->acc + i] = r;
			}
		}

		m4a->acc += take;
		src += (size_t)take * (size_t)bytes_per_frame;
		offset += take;

		if (m4a->acc == m4a->frame_cap) {
			flush_current_frame(m4a, m4a->frame_cap);
		}
	}
}

static void
m4a_writer_free(struct m4a_writer *m4a)
{
	if (m4a == NULL) {
		return;
	}
	if (m4a->fmt_ctx != NULL && m4a->fmt_ctx->pb != NULL) {
		av_freep(&m4a->fmt_ctx->pb->buffer);
		avio_context_free(&m4a->fmt_ctx->pb);
	}
	av_packet_free(&m4a->pkt);
	av_frame_free(&m4a->frame);
	avcodec_free_context(&m4a->codec_ctx);
	avformat_free_context(m4a->fmt_ctx);
	free(m4a);
}

static void
m4a_destroy(struct audio_writer *writer)
{
	struct m4a_writer *m4a;
	int ret;

	assert(writer != NULL);
	m4a = writer->context;

	if (m4a->acc > 0) {
		if (m4a->codec_ctx->codec->capabilities &
		    AV_CODEC_CAP_SMALL_LAST_FRAME) {
			flush_current_frame(m4a, m4a->acc);
		} else {
			float *ch0 = (float *)m4a->frame->data[0];
			float *ch1 = (m4a->out_channels > 1) ?
			    (float *)m4a->frame->data[1] : NULL;
			int rem = m4a->frame_cap - m4a->acc;

			memset(ch0 + m4a->acc, 0, (size_t)rem * sizeof(float));
			if (ch1 != NULL) {
				memset(ch1 + m4a->acc, 0,
				    (size_t)rem * sizeof(float));
			}
			flush_current_frame(m4a, m4a->frame_cap);
		}
	}

	ret = avcodec_send_frame(m4a->codec_ctx, NULL);
	if (ret < 0 && ret != AVERROR_EOF) {
		char err[AV_ERROR_MAX_STRING_SIZE];

		av_strerror(ret, err, sizeof err);
		fprintf(stderr, "avcodec_send_frame flush failed: %s\n", err);
	} else {
		drain_encoder_packets(m4a);
	}

	ret = av_write_trailer(m4a->fmt_ctx);
	if (ret < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE];

		av_strerror(ret, err, sizeof err);
		fprintf(stderr, "av_write_trailer failed: %s\n", err);
	}
	if (m4a->fmt_ctx->pb != NULL) {
		avio_flush(m4a->fmt_ctx->pb);
	}
	fflush(m4a->outfile);

	m4a_writer_free(m4a);
	free(writer);
}

struct audio_writer *
audio_writer_m4a(FILE *outfile, int sample_rate, int nchannels, int mono)
{
	struct audio_writer *writer;
	struct m4a_writer *m4a;
	const AVCodec *codec;
	unsigned char *avio_buf;
	AVIOContext *avio;
	int out_ch, ret;

	assert(outfile != NULL);
	assert(sample_rate > 0);
	assert(nchannels == 1 || nchannels == 2);

	if (fflush(outfile) != 0 || fseeko(outfile, 0, SEEK_SET) != 0) {
		perror("fseeko");
		return NULL;
	}

	av_log_set_level(AV_LOG_ERROR);

	codec = avcodec_find_encoder_by_name("aac");
	if (codec == NULL) {
		codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	}
	if (codec == NULL || !encoder_supports_fltp(codec)) {
		fprintf(stderr, "FFmpeg built-in AAC FLTP encoder unavailable\n");
		return NULL;
	}

	writer = malloc(sizeof *writer);
	m4a = calloc(1, sizeof *m4a);
	if (writer == NULL || m4a == NULL) {
		fprintf(stderr, "No memory for M4A audio writer\n");
		free(writer);
		free(m4a);
		exit(EXIT_FAILURE);
	}

	m4a->outfile = outfile;
	m4a->in_channels = nchannels;
	out_ch = mono ? 1 : nchannels;
	m4a->out_channels = out_ch;

	ret = avformat_alloc_output_context2(&m4a->fmt_ctx, NULL, "ipod",
	    "castty.m4a");
	if (ret < 0 || m4a->fmt_ctx == NULL) {
		fprintf(stderr, "Couldn't allocate M4A muxer context\n");
		goto fail;
	}

	m4a->codec_ctx = avcodec_alloc_context3(codec);
	if (m4a->codec_ctx == NULL) {
		goto fail;
	}
	m4a->codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
	m4a->codec_ctx->sample_rate = sample_rate;
	m4a->codec_ctx->bit_rate = (out_ch == 1) ? 96000 : 128000;
	m4a->codec_ctx->time_base = (AVRational){ 1, sample_rate };
	av_channel_layout_default(&m4a->codec_ctx->ch_layout, out_ch);
	if (m4a->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
		m4a->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	ret = avcodec_open2(m4a->codec_ctx, codec, NULL);
	if (ret < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE];

		av_strerror(ret, err, sizeof err);
		fprintf(stderr, "Couldn't open AAC encoder (%d Hz): %s\n",
		    sample_rate, err);
		goto fail;
	}

	m4a->stream = avformat_new_stream(m4a->fmt_ctx, NULL);
	if (m4a->stream == NULL) {
		goto fail;
	}
	m4a->stream->id = 0;
	m4a->stream->time_base = m4a->codec_ctx->time_base;
	if (avcodec_parameters_from_context(m4a->stream->codecpar,
	    m4a->codec_ctx) < 0) {
		goto fail;
	}

	avio_buf = av_malloc(M4A_AVIO_BUFSZ);
	if (avio_buf == NULL) {
		goto fail;
	}
	avio = avio_alloc_context(avio_buf, M4A_AVIO_BUFSZ, 1, outfile, NULL,
	    stdio_write_packet, stdio_seek);
	if (avio == NULL) {
		av_free(avio_buf);
		goto fail;
	}
	m4a->fmt_ctx->pb = avio;
	m4a->fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

	m4a->frame_cap = (m4a->codec_ctx->frame_size > 0) ?
	    m4a->codec_ctx->frame_size : 1024;
	m4a->frame = av_frame_alloc();
	m4a->pkt = av_packet_alloc();
	if (m4a->frame == NULL || m4a->pkt == NULL) {
		goto fail;
	}
	m4a->frame->format = m4a->codec_ctx->sample_fmt;
	m4a->frame->sample_rate = m4a->codec_ctx->sample_rate;
	m4a->frame->nb_samples = m4a->frame_cap;
	if (av_channel_layout_copy(&m4a->frame->ch_layout,
	    &m4a->codec_ctx->ch_layout) < 0 ||
	    av_frame_get_buffer(m4a->frame, 0) < 0) {
		goto fail;
	}

	ret = avformat_write_header(m4a->fmt_ctx, NULL);
	if (ret < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE];

		av_strerror(ret, err, sizeof err);
		fprintf(stderr, "avformat_write_header failed: %s\n", err);
		goto fail;
	}

	writer->context = m4a;
	writer->write = m4a_write;
	writer->destroy = m4a_destroy;
	return writer;

fail:
	m4a_writer_free(m4a);
	free(writer);
	return NULL;
}

static float
read_avframe_sample(const AVFrame *frame, int ch, int idx)
{
	switch ((enum AVSampleFormat)frame->format) {
	case AV_SAMPLE_FMT_FLTP:
		return ((const float *)frame->extended_data[ch])[idx];
	case AV_SAMPLE_FMT_FLT: {
		int nch = frame->ch_layout.nb_channels;

		return ((const float *)frame->data[0])[idx * nch + ch];
	}
	case AV_SAMPLE_FMT_S16P:
		return (float)((const int16_t *)frame->extended_data[ch])[idx] /
		    32768.0f;
	case AV_SAMPLE_FMT_S16: {
		int nch = frame->ch_layout.nb_channels;

		return (float)((const int16_t *)frame->data[0])[idx * nch + ch] /
		    32768.0f;
	}
	case AV_SAMPLE_FMT_S32P:
		return (float)((double)((const int32_t *)frame->extended_data[ch])[idx] /
		    2147483648.0);
	case AV_SAMPLE_FMT_S32: {
		int nch = frame->ch_layout.nb_channels;

		return (float)((double)((const int32_t *)frame->data[0])[idx * nch + ch] /
		    2147483648.0);
	}
	case AV_SAMPLE_FMT_DBLP:
		return (float)((const double *)frame->extended_data[ch])[idx];
	case AV_SAMPLE_FMT_DBL: {
		int nch = frame->ch_layout.nb_channels;

		return (float)((const double *)frame->data[0])[idx * nch + ch];
	}
	default:
		return 0.0f;
	}
}

static int
open_m4a_decoder(const char *path, AVFormatContext **fmt_out,
    AVCodecContext **dec_out, int *stream_idx_out, char *errbuf, size_t errsz)
{
	AVFormatContext *fmt_ctx = NULL;
	AVCodecContext *dec_ctx = NULL;
	const AVCodec *dec = NULL;
	int idx, ret;

	av_log_set_level(AV_LOG_ERROR);
	ret = avformat_open_input(&fmt_ctx, path, NULL, NULL);
	if (ret < 0) {
		char av_err[AV_ERROR_MAX_STRING_SIZE];

		av_strerror(ret, av_err, sizeof av_err);
		snprintf(errbuf, errsz, "castty: %s is not a valid m4a file: %s",
		    path, av_err);
		return -1;
	}

	if (fmt_ctx->iformat == NULL || fmt_ctx->iformat->name == NULL ||
	    (strstr(fmt_ctx->iformat->name, "mp4") == NULL &&
	    strstr(fmt_ctx->iformat->name, "m4a") == NULL)) {
		snprintf(errbuf, errsz, "castty: %s is not in m4a/mp4 format",
		    path);
		avformat_close_input(&fmt_ctx);
		return -1;
	}

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
		snprintf(errbuf, errsz,
		    "castty: %s contains no readable streams", path);
		avformat_close_input(&fmt_ctx);
		return -1;
	}

	idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
	if (idx < 0 || dec == NULL) {
		snprintf(errbuf, errsz, "castty: %s contains no audio stream",
		    path);
		avformat_close_input(&fmt_ctx);
		return -1;
	}

	dec_ctx = avcodec_alloc_context3(dec);
	if (dec_ctx == NULL ||
	    avcodec_parameters_to_context(dec_ctx,
	    fmt_ctx->streams[idx]->codecpar) < 0 ||
	    avcodec_open2(dec_ctx, dec, NULL) < 0) {
		snprintf(errbuf, errsz,
		    "castty: couldn't open decoder for %s", path);
		avcodec_free_context(&dec_ctx);
		avformat_close_input(&fmt_ctx);
		return -1;
	}

	*fmt_out = fmt_ctx;
	*dec_out = dec_ctx;
	*stream_idx_out = idx;
	return 0;
}

static int
run_m4a_decode(const char *path, int stop_after_first, audio_m4a_sample_cb cb,
    void *userdata, int *rate_out, int *stereo_out, char *errbuf, size_t errsz)
{
	AVFormatContext *fmt_ctx = NULL;
	AVCodecContext *dec_ctx = NULL;
	AVPacket *pkt = NULL;
	AVFrame *frame = NULL;
	int stream_idx = -1, got_frames = 0, rate = 0, nch = 0, rc = -1;

	if (open_m4a_decoder(path, &fmt_ctx, &dec_ctx, &stream_idx, errbuf,
	    errsz) != 0) {
		return -1;
	}

	pkt = av_packet_alloc();
	frame = av_frame_alloc();
	if (pkt == NULL || frame == NULL) {
		snprintf(errbuf, errsz, "castty: out of memory decoding %s",
		    path);
		goto out;
	}

	for (int flushing = 0;;) {
		int rret;

		if (!flushing) {
			rret = av_read_frame(fmt_ctx, pkt);
			if (rret == AVERROR_EOF) {
				flushing = 1;
				(void)avcodec_send_packet(dec_ctx, NULL);
			} else if (rret < 0) {
				snprintf(errbuf, errsz,
				    "castty: %s read error during decode", path);
				goto out;
			} else if (pkt->stream_index != stream_idx) {
				av_packet_unref(pkt);
				continue;
			} else {
				int sret = avcodec_send_packet(dec_ctx, pkt);

				av_packet_unref(pkt);
				if (sret < 0) {
					snprintf(errbuf, errsz,
					    "castty: %s contains corrupt audio frames",
					    path);
					goto out;
				}
			}
		}

		for (;;) {
			int dret = avcodec_receive_frame(dec_ctx, frame);
			int fnch, i;

			if (dret == AVERROR(EAGAIN) || dret == AVERROR_EOF) {
				break;
			}
			if (dret < 0) {
				snprintf(errbuf, errsz,
				    "castty: %s decode error", path);
				goto out;
			}

			fnch = frame->ch_layout.nb_channels;
			if (frame->sample_rate <= 0 || fnch <= 0) {
				av_frame_unref(frame);
				continue;
			}
			rate = frame->sample_rate;
			nch = fnch;
			if (frame->nb_samples > 0) {
				got_frames = 1;
			}

			if (cb != NULL) {
				for (i = 0; i < frame->nb_samples; i++) {
					float sl = read_avframe_sample(frame, 0, i);
					float sr = (fnch > 1) ?
					    read_avframe_sample(frame, 1, i) : sl;

					cb(userdata, rate, sl, sr);
				}
			}
			av_frame_unref(frame);

			if (stop_after_first && got_frames) {
				goto done;
			}
		}

		if (flushing) {
			break;
		}
	}

done:
	if (!got_frames || rate <= 0 || nch <= 0) {
		snprintf(errbuf, errsz,
		    "castty: %s contains no valid audio frames", path);
		goto out;
	}
	if (rate_out != NULL) {
		*rate_out = rate;
	}
	if (stereo_out != NULL) {
		*stereo_out = (nch >= 2);
	}
	rc = 0;

out:
	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&dec_ctx);
	avformat_close_input(&fmt_ctx);
	return rc;
}

int
audio_m4a_probe(const char *path, int *rate_out, int *stereo_out,
    char *errbuf, size_t errsz)
{
	return run_m4a_decode(path, 1, NULL, NULL, rate_out, stereo_out, errbuf,
	    errsz);
}

int
audio_m4a_decode(const char *path, audio_m4a_sample_cb cb, void *userdata,
    char *errbuf, size_t errsz)
{
	return run_m4a_decode(path, 0, cb, userdata, NULL, NULL, errbuf, errsz);
}

/* See LICENSE for redistribution information */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include <pthread.h>

#include <ck_ring.h>
#include <portaudio.h>

#include "castty.h"

#ifndef MIN
#define MIN(a, b) ((a < b) ? a : b)
#endif

static PaStream *stream;
static PaStreamParameters inParams;
static const PaDeviceInfo *cdev;

struct data {
	unsigned	fidx;
	ck_ring_t	ring;
	FILE		*file;

	ck_ring_buffer_t *b;
} buffer;

static int post = 2;
static int record;

pthread_t wthread, rthread;

static void *
writer(void *v)
{
	(void)v;

	__sync_fetch_and_sub(&post, 1);
	while (post > 0) {
		usleep(1000);
	}

	while (1) {
		intptr_t v;

		if (ck_ring_dequeue_spsc(&buffer.ring, buffer.b, &v)) {
			int16_t av = (int16_t)v;
			fwrite(&av, sizeof av, 1, buffer.file);
		} else {
			usleep(2000);
		}

		if (post > 0) {
			break;
		}
	}

	return NULL;
}

static int
audio_record(const void *ibuf, void *obuf, unsigned long frames,
    const PaStreamCallbackTimeInfo* tinfo, PaStreamCallbackFlags sflags,
    void *opaque)
{
	const int16_t *buf = ibuf;

	(void)obuf;
	(void)tinfo;
	(void)sflags;
	(void)opaque;

	for (unsigned long i = 0; i < frames * 2; i++) {
		intptr_t v = buf[i];
		ck_ring_enqueue_spsc(&buffer.ring, buffer.b, (void *)v);
	}

	return paContinue;
}

static unsigned
np2(unsigned val)
{
    val--;
    val = (val >> 1) | val;
    val = (val >> 2) | val;
    val = (val >> 4) | val;
    val = (val >> 8) | val;
    val = (val >> 16) | val;
    return ++val;
}

static void *
reader(void *v)
{
	PaError err;

	(void) v;

	err = Pa_Initialize();
	if (err != paNoError) {
		fprintf(stderr, "Pa_Initialize: %s\n", Pa_GetErrorText(err));
		exit(EXIT_FAILURE);
	}

	int ndev;

	ndev = Pa_GetDeviceCount();
	if (ndev < 0) {
		fprintf(stderr, "Pa_GetDeviceCount: %s\n",
		    Pa_GetErrorText(err));
		exit(EXIT_FAILURE);
	} else if (ndev == 0) {
		fprintf(stderr, "No audio devices found!\n");
		exit(EXIT_FAILURE);
	}

	while (1) {
		int d;

		for (int i = 0; i < ndev; i++) {
			const PaDeviceInfo *dev;

			dev = Pa_GetDeviceInfo(i);
			if (dev->maxInputChannels) {
				printf("%d:\t%s (%d ch)\n", i, dev->name,
				    dev->maxInputChannels);
			}
		}

		printf("Select input device: ");
		fflush(stdout);

		int r = scanf("%d", &d);
		if (r != 1) {
			perror("scanf");
			exit(EXIT_FAILURE);
		}

		if (d > ndev) {
			fprintf(stderr, "Invalid selection\n");
		}
		cdev = Pa_GetDeviceInfo(d);
		if (!cdev->maxInputChannels) {
			fprintf(stderr, "Invalid selection\n");
		} else {
			break;
		}
	}

	/* 44100Hz * 16 bit samples * 2 channels */
	buffer.b = calloc(np2(44100 * 4), sizeof buffer.b);
	if (buffer.b == NULL) {
		fprintf(stderr, "Couldn't allocate ring buffer\n");
		exit(EXIT_FAILURE);
	}

	ck_ring_init(&buffer.ring, np2(44100 * 2));

	inParams.channelCount = MIN(2, cdev->maxInputChannels);
	inParams.sampleFormat = paInt16;
	inParams.suggestedLatency = cdev->defaultLowInputLatency;

	err = Pa_OpenStream(&stream, &inParams, NULL, 44100, 1024, paClipOff,
	    audio_record, NULL);
	if (err != paNoError) {
		fprintf(stderr, "Pa_OpenStream: %s\n", Pa_GetErrorText(err));
		exit(EXIT_FAILURE);
	}

	__sync_fetch_and_sub(&post, 1);
	while (post > 0 || record == 0) {
		usleep(10);
	}

	err = Pa_StartStream(stream);
	if (err != paNoError) {
		fprintf(stderr, "Pa_StartStream: %s\n", Pa_GetErrorText(err));
		exit(EXIT_FAILURE);
	}

	while (1) {
		if (record == 0 && Pa_IsStreamActive(stream) == 1) {
			err = Pa_StopStream(stream);

			if (err != paNoError) {
				fprintf(stderr, "Pa_StopStream: %s\n",
				    Pa_GetErrorText(err));
				exit(EXIT_FAILURE);
			}
		} else if (record == 1 && Pa_IsStreamStopped(stream) == 1) {
			err = Pa_StartStream(stream);

			if (err != paNoError) {
				fprintf(stderr, "Pa_StartStream: %s\n",
				    Pa_GetErrorText(err));
				exit(EXIT_FAILURE);
			}
		}

		if (post) {
			if (Pa_IsStreamActive(stream) == 1) {
				err = Pa_StopStream(stream);
				if (err != paNoError) {
					fprintf(stderr, "Pa_StopStream: %s\n",
					    Pa_GetErrorText(err));
					exit(EXIT_FAILURE);
				}
			}

			break;
		}

		usleep(1000);
	}

	return NULL;
}

void
audio_toggle(void)
{
	record = !record;
}

void
audio_init(const char *file, int flag)
{

	buffer.file = fopen(file, flag ? "ab" : "wb");
	if (buffer.file == NULL) {
		perror("fopen");
		exit(EXIT_FAILURE);
	}

	if (pthread_create(&wthread, NULL, writer, NULL) != 0) {
		perror("pthread_create");
		exit(EXIT_FAILURE);
	}

	if (pthread_create(&rthread, NULL, reader, NULL) != 0) {
		perror("pthread_create");
		exit(EXIT_FAILURE);
	}

	while (post) {
		usleep(1000);
	}
}

void
audio_deinit(void)
{
	PaError err;

	post++;
	pthread_join(wthread, NULL);
	pthread_join(rthread, NULL);

	err = Pa_Terminate();
	if (err != paNoError) {
		fprintf(stderr, "Pa_Terminate: %s\n", Pa_GetErrorText(err));
		exit(EXIT_FAILURE);
	}
}

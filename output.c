#include <sys/time.h>

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "castty.h"
#include "utf8.h"

static int audio_enabled, paused, start_paused;
static struct timeval prevtv, nowtv;
static double aprev, anow, dur;
static FILE *evout;
static int master;

static void
handle_command(enum control_command cmd)
{
	static unsigned char c_a = 0x01;
	static unsigned char c_l = 0x0c;

	switch (cmd) {
	case CMD_CTRL_A:
		/* Can't just write this to STDOUT_FILENO; this has to be
		 * written to the master end of the tty, otherwise it's 
		 * ignored.
		 */
		xwrite(master, &c_a, 1);
		break;

	case CMD_MUTE:
		if (audio_enabled) {
			audio_toggle_mute();
		}
		break;

	case CMD_PAUSE:
		paused = !paused;
		if (!paused) {
			/* Redraw screen */
			xwrite(master, &c_l, 1);
			if (audio_enabled) {
				audio_start();
				anow = aprev = audio_clock_ms();
			} else {
				gettimeofday(&prevtv, NULL);
				nowtv = prevtv;
			}
		} else {
			if (audio_enabled) {
				audio_stop();
			}
		}
		break;

	default:
		abort();
	}
}

static void
handle_input(unsigned char *buf, size_t buflen)
{
	static int first = 1;
	double delta;

	if (first) {
		if (audio_enabled) {
			if (!start_paused) {
				audio_start();
			}
			anow = aprev = audio_clock_ms();
		} else {
			gettimeofday(&prevtv, NULL);
			nowtv = prevtv;
		}

		first = 0;
	} else {
		if (audio_enabled) {
			anow = audio_clock_ms();
		} else {
			gettimeofday(&nowtv, NULL);
		}
	}

	if (audio_enabled) {
		delta = anow - aprev;
		aprev = anow;
	} else {
		double pms, nms;

		pms = (double)(prevtv.tv_sec * 1000) + ((double)prevtv.tv_usec / 1000.);
		nms = (double)(nowtv.tv_sec * 1000) + ((double)nowtv.tv_usec / 1000.);

		delta = nms - pms;
		prevtv = nowtv;
	}

	dur += delta;

	fprintf(evout, ",[%0.4f,\"", delta / 1000);

	uint32_t state, cp;
	state = 0;
	for (size_t j = 0; j < buflen; j++) {
		if (!u8_decode(&state, &cp, buf[j])) {
			if ((cp < 128 && !isprint(cp)) ||
			    cp > 128) {
				if (state == UTF8_ACCEPT) {
					fprintf(evout, "\\u%04" PRIx32, cp);
				} else {
					fputs("\\u1f4a9", evout);
				}
			} else {
				switch (buf[j]) {
				case '"':
				case '\\':
					fputc('\\', evout);
				default:
					fputc(buf[j], evout);
					break;
				}
			}
		}
	}

	fputs("\"]", evout);
}

void
outputproc(struct outargs *oa)
{
	unsigned char obuf[BUFSIZ];
	struct pollfd pollfds[2];
	int status;

	status = EXIT_SUCCESS;
	master = oa->masterfd;

	if (oa->audioout || oa->devid) {
		assert(oa->audioout && oa->devid);
	}

	if (oa->audioout) {
		audio_enabled = 1;
		audio_init(oa->devid, oa->audioout);
	}

	start_paused = paused = oa->start_paused;

	evout = xfopen(oa->outfn, "wb");

	/* Write asciicast v1 header and append events. Format defined at
	 * https://github.com/asciinema/asciinema/blob/master/doc/asciicast-v1.md
	 *
	 * We insert an empty first record to avoid the hassle of dealing with
	 * ES (still) not supporting trailing commas.
	 */
	fprintf(evout,
	    "{\n"
	    " \"version\": 1,\n"
	    " \"width\": %d,\n"
	    " \"height\": %d,\n"
	    " \"command\": \"%s\",\n"
	    " \"title\": \"%s\",\n"
	    " \"env\": %s,\n"
	    " \"stdout\":[[0,\"\"]",
	    oa->cols, oa->rows,
	    oa->cmd ? oa->cmd : "",
	    oa->title ? oa->title : "",
	    oa->env);

	setbuf(evout, NULL);
	setbuf(stdout, NULL);

	xclose(STDIN_FILENO);

	/* Clear screen */
	printf("\x1b[2J");

	/* Move cursor to top-left */
	printf("\x1b[H");

	int f = fcntl(oa->masterfd, F_GETFL);
	fcntl(oa->masterfd, F_SETFL, f | O_NONBLOCK);

	f = fcntl(oa->controlfd, F_GETFL);
	fcntl(oa->controlfd, F_SETFL, f | O_NONBLOCK);

	/* Control descriptor is highest priority */
	pollfds[0].fd = oa->controlfd;
	pollfds[0].events = POLLIN;
	pollfds[0].revents = 0;

	pollfds[1].fd = oa->masterfd;
	pollfds[1].events = POLLIN;
	pollfds[1].revents = 0;

	for (;;) {
		int nready;

		nready = poll(pollfds, 2, -1);
		if (nready == -1 && errno == EINTR) {
			continue;
		} else if (nready == -1) {
			perror("poll");
			status = EXIT_FAILURE;
			goto end;
		}

		for (int i = 0; i < 2; i++) {
			if ((pollfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))) {
				status = EXIT_FAILURE;
				goto end;
			}

			if (!(pollfds[i].revents & POLLIN)) {
				continue;
			}

			if (pollfds[i].fd == oa->controlfd) {
				enum control_command cmd;
				ssize_t nread;

				nread = read(oa->controlfd, &cmd, sizeof cmd);
				if (nread == -1 || nread != sizeof cmd) {
					perror("read");
					status = EXIT_FAILURE;
					goto end;
				}

				handle_command(cmd);
			} else if (pollfds[i].fd == oa->masterfd) {
				ssize_t nread;

				nread = read(oa->masterfd, obuf, BUFSIZ);
				if (nread <= 0) {
					status = EXIT_FAILURE;
					goto end;
				}

				xwrite(STDOUT_FILENO, obuf, nread);

				if (!paused) {
					handle_input(obuf, nread);
				}
			}
		}
	}

end:
	fprintf(evout, "],\n \"duration\":%0.3f}", dur);

	fflush(evout);

	if (oa->audioout && oa->devid) {
		audio_stop();
		audio_exit();
	}

	xfclose(evout);
	xclose(oa->masterfd);

	exit(status);
}

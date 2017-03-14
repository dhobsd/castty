#include <sys/time.h>

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "castty.h"

static struct timeval prevtv, nowtv;
static int paused, audio_enabled;
static double aprev, anow, dur;
static FILE *evout;
static int master;

static void
handle_command(enum control_command cmd)
{
	static char c_a = 0x01;

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
			if (audio_enabled) {
				anow = aprev = audio_clock_ms();
				audio_start();
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
handle_input(char *buf, size_t buflen)
{
	static int first = 1;

	xwrite(STDOUT_FILENO, buf, buflen);

	if (first) {
		if (audio_enabled) {
			audio_start();
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
		dur += anow - aprev;
		aprev = anow;
	} else {
		double pms, nms;

		pms = (double)(prevtv.tv_sec * 1000) + ((double)prevtv.tv_usec / 1000.);
		nms = (double)(nowtv.tv_sec * 1000) + ((double)nowtv.tv_usec / 1000.);

		dur += nms - pms;
		prevtv = nowtv;
	}

	fprintf(evout, "\"s\":%0.3f,\"e\":\"", dur);

	for (size_t j = 0; j < buflen; j++) {
		switch (buf[j]) {
			case '"':
			case '\\':
			case '%':
			      fprintf(evout, "%%%02hhx", buf[j]);
			      break;
			default:
				if (!isprint(buf[j])) {
					fprintf(evout, "%%%02hhx", buf[j]);
				} else {
					fputc(buf[j], evout);
				}
				break;
		}
	}

	fputs("\"},{", evout);
}

void
outputproc(int masterfd, int controlfd, const char *outfn, const char *audioout,
    const char *devid, int append, int rows, int cols)
{
	struct pollfd pollfds[2];
	char obuf[BUFSIZ];
	int status;

	status = EXIT_SUCCESS;
	master = masterfd;

	if (audioout || devid) {
		assert(audioout && devid);
	}

	if (audioout) {
		audio_enabled = 1;
		audio_init(devid, audioout);
	}

	evout = fopen(outfn, append ? "ab" : "wb");
	if (evout == NULL) {
		perror("fopen");
		status = EXIT_FAILURE;
		goto end;
	}

	setbuf(evout, NULL);
	setbuf(stdout, NULL);

	xclose(STDIN_FILENO);

	/* Clear screen */
	printf("\x1b[2J");

	/* Move cursor to top-left */
	printf("\x1b[H");

	fprintf(evout, "var _ti={\"rows\":%d,\"cols\":%d};\n", rows, cols);
	fputs("var _tre=[{", evout);

	int f = fcntl(masterfd, F_GETFL);
	fcntl(masterfd, F_SETFL, f | O_NONBLOCK);

	f = fcntl(controlfd, F_GETFL);
	fcntl(controlfd, F_SETFL, f | O_NONBLOCK);

	/* Control descriptor is highest priority */
	pollfds[0].fd = controlfd;
	pollfds[0].events = POLLIN;
	pollfds[0].revents = 0;

	pollfds[1].fd = masterfd;
	pollfds[1].events = POLLIN;
	pollfds[1].revents = 0;

	for (;;) {
		int nready;

		nready = poll(pollfds, 2, -1);
		if (nready == -1) {
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

			if (pollfds[i].fd == controlfd) {
				enum control_command cmd;
				ssize_t nread;

				nread = read(controlfd, &cmd, sizeof cmd);
				if (nread == -1 || nread != sizeof cmd) {
					perror("read");
					status = EXIT_FAILURE;
					goto end;
				}

				handle_command(cmd);
			} else if (pollfds[i].fd == masterfd) {
				ssize_t nread;

				nread = read(masterfd, obuf, BUFSIZ);
				if (nread <= 0) {
					status = EXIT_FAILURE;
					goto end;
				}

				if (!paused) {
					handle_input(obuf, nread);
				}
			}
		}
	}

end:
	if (audioout && devid) {
		audio_stop();
		audio_exit();
	}

	/* Empty last record */
	fprintf(evout, "\"s\":%0.3f,\"e\":\"\"}];", dur);

	xfclose(evout);
	xclose(masterfd);

	exit(status);
}

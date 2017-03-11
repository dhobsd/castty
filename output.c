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

static double
time_delta(struct timeval *prev, struct timeval *now)
{
	double pms, nms;

	pms = (double)(prev->tv_sec * 1000) + ((double)prev->tv_usec / 1000.);
	nms = (double)(now->tv_sec * 1000) + ((double)now->tv_usec / 1000.);

	return nms - pms;
}

static struct command {
	unsigned char cmdbuf[BUFSIZ];
	unsigned off;
} command;

static void
handle_command(void)
{

	return;
}

void
outputproc(int masterfd, int controlfd, const char *outfn, const char *audioout,
    int append, int rows, int cols)
{
	static struct timeval prev, now;
	struct pollfd pollfds[2];
	static double dur;
	char obuf[BUFSIZ];
	int paused = 0;
	int first = 1;
	FILE *evout;
	int cc;

	if (audioout) {
		audio_start(audioout, append);
	}

	evout = fopen(outfn, append ? "ab" : "wb");
	if (evout == NULL) {
		perror("fopen");
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

	if (audioout) {
		audio_toggle();
	}

	for (;;) {
		int nready;

		nready = poll(pollfds, 2, -1);
		if (nready == -1) {
			perror("poll");
			goto end;
		}

		for (int i = 0; i < 2; i++) {
			if ((pollfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))) {
				goto end;
			}

			if (!(pollfds[i].revents & POLLIN)) {
				continue;
			}

			if (pollfds[i].fd == controlfd) {
				unsigned char *epos;

				if (command.off == 0) {
					printf("\x1b" "7\x1b[0;37;1;40m<cmd> ");
				}

				cc = read(controlfd, command.cmdbuf + command.off,
				    sizeof command.cmdbuf - command.off);
				if (cc == -1) {
					perror("read");
					goto end;
				}

				epos = memchr(command.cmdbuf + command.off, '\r', cc);
				if (!epos) {
					printf("%.*s", cc, command.cmdbuf + command.off);
				} else {
					unsigned char *p = command.cmdbuf + command.off;

					while (p < epos) {
						fputc(*p++, stdout);
					}
				}

				command.off += cc;
				assert(command.off < sizeof command.cmdbuf);

				if (command.cmdbuf[command.off - 1] == '\r' ||
				    command.cmdbuf[command.off - 1] == '\n') {
					printf("\x1b" "8\x1b" "0\x1b[K");
					handle_command();
					command.off = 0;
				}
			} else if (pollfds[i].fd == masterfd) {
				cc = read(masterfd, obuf, BUFSIZ);
				if (cc <= 0) {
					goto end;
				}

				xwrite(STDOUT_FILENO, obuf, cc);

				if (!paused) {
					if (first) {
						gettimeofday(&prev, NULL);
						now = prev;
						first = 0;
					} else {
						gettimeofday(&now, NULL);
					}

					dur += time_delta(&prev, &now);
					prev = now;

					fprintf(evout, "\"s\":%0.3f,\"e\":\"", dur);

					for (int j = 0; j < cc; j++) {
						switch (obuf[j]) {
							case '"':
							case '\\':
								fputc('\\', evout);
							default:
								if (!isprint(obuf[j])) {
									fprintf(evout, "\\x%02hhx", obuf[j]);
								} else {
									fputc(obuf[j], evout);
								}
								break;
						}
					}

					fputs("\"},{", evout);
				}
			}
		}
	}

end:
	if (audioout) {
		audio_deinit();
	}

	/* Empty last record */
	fprintf(evout, "\"s\":%0.3f,\"e\":\"\"}];", dur);
	fflush(evout);

	xfclose(evout);
	xclose(masterfd);

	return;
}

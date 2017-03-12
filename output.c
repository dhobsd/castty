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

struct cmdinput {
	char buf[BUFSIZ];
	unsigned off;
};

enum command {
	CMD_MUTE,
	CMD_PAUSE,
	CMD_NONE,
};

struct {
	enum command cmd;
	char *str;
} commands[] = {
	{ CMD_MUTE, ":mute", },
	{ CMD_PAUSE, ":pause", },
};

static enum command
get_command(struct cmdinput *cmd)
{

	for (unsigned i = 0; i < sizeof commands / sizeof commands[0]; i++) {
		if (strcmp(commands[i].str, cmd->buf) == 0) {
			return commands[i].cmd;
		}
	}

	return CMD_NONE;
}


static double
time_delta(struct timeval *prev, struct timeval *now)
{
	double pms, nms;

	pms = (double)(prev->tv_sec * 1000) + ((double)prev->tv_usec / 1000.);
	nms = (double)(now->tv_sec * 1000) + ((double)now->tv_usec / 1000.);

	return nms - pms;
}

void
outputproc(int masterfd, int controlfd, const char *outfn, const char *audioout,
    int append, int rows, int cols)
{
	static struct timeval prev, now;
	struct pollfd pollfds[2];
	struct cmdinput cmd;
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

	cmd.off = 0;

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
				char *epos;

				if (cmd.off == 0) {
					printf("\x1b" "7\x1b[0;37;1;40m<cmd> ");
				}

				cc = read(controlfd, cmd.buf + cmd.off,
				    sizeof cmd.buf - cmd.off);
				if (cc == -1) {
					perror("read");
					goto end;
				}

				epos = memchr(cmd.buf + cmd.off, '\r', cc);
				if (!epos) {
					printf("%.*s", cc, cmd.buf + cmd.off);
				} else {
					char *p = cmd.buf + cmd.off;

					while (p < epos) {
						fputc(*p++, stdout);
					}
				}

				cmd.off += cc;
				assert(cmd.off < sizeof cmd.buf);

				if (cmd.buf[cmd.off - 1] == '\r') {
					enum command command;

					cmd.buf[cmd.off - 1] = '\0';
					cmd.off = 0;

					command = get_command(&cmd);
					switch (command) {
					case CMD_MUTE:
						audio_toggle_mute();
						break;

					case CMD_PAUSE:
						paused = !paused;
						audio_toggle_pause();
						if (!paused) {
							gettimeofday(&prev, NULL);
							now = prev;
						}
						break;

					case CMD_NONE:
						break;

					default:
						abort();
					}

					printf("\x1b" "8\x1b" "0\x1b[K");
				}
			} else if (pollfds[i].fd == masterfd) {
				if (audioout && first) {
					audio_toggle_pause();
				}

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
		audio_stop();
	}

	/* Empty last record */
	fprintf(evout, "\"s\":%0.3f,\"e\":\"\"}];", dur);
	fflush(evout);

	xfclose(evout);
	xclose(masterfd);

	return;
}

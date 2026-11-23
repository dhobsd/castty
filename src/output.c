#include <sys/time.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "audio.h"
#include "castty.h"
#include "record.h"
#include "utf8.h"

static int audio_enabled, paused, start_paused;
static int first = 1;
static struct timeval prevtv, nowtv;
static double aprev, anow, dur;
static FILE *evout;
static int master;

/*
 * Always drive timing from the audio sample clock when recording audio so
 * terminal events stay locked to the audio track without drift. Muting (^a m)
 * still records zeroed silence frames and keeps advancing audio_clock_ms(),
 * so we never switch clocks mid-recording.
 */
static double
next_delta_ms(int start_audio)
{
	double delta;

	if (first) {
		if (audio_enabled) {
			if (start_audio && !start_paused) {
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
	return delta;
}

static void
handle_command(const struct control_msg *msg, int format_version)
{
	static unsigned char c_l = 0x0c;

	switch (msg->cmd) {
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

	case CMD_RESIZE:
		if (!paused && (format_version == 2 || format_version == 3)) {
			double rdelta = next_delta_ms(1);
			double rtime = (format_version == 3) ? rdelta : dur;
			fprintf(evout, "[%0.4f,\"r\",\"%hux%hu\"]\n",
			    rtime / 1000, msg->cols, msg->rows);
		}
		break;

	default:
		abort();
	}
}

static void
handle_input(unsigned char *buf, size_t buflen, int format_version)
{
	assert(format_version == 1 || format_version == 2 ||
	    format_version == 3);
	double delta = next_delta_ms(1);

	if (format_version == 3) {
		fprintf(evout, "[%0.4f,\"o\",\"", delta / 1000);
	} else if (format_version == 2) {
		fprintf(evout, "[%0.4f,\"o\",\"", dur / 1000);
	} else if (format_version == 1) {
		fprintf(evout, ",[%0.4f,\"", delta / 1000);
	}

	uint32_t state, cp;
	state = 0;
	for (size_t j = 0; j < buflen; j++) {
		if (!u8_decode(&state, &cp, buf[j])) {
			if ((cp < 128 && !isprint(cp)) ||
			    cp > 128) {
				if (state == UTF8_ACCEPT) {
					if (cp > 0xffff) {
						uint32_t h, l;
						h = ((cp - 0x10000) >> 10) + 0xd800;
						l = ((cp - 0x10000) & 0x3ff) + 0xdc00;
						fprintf(evout, "\\u%04" PRIx32 "\\u%04" PRIx32, h, l);
					} else {
						fprintf(evout, "\\u%04" PRIx32, cp);
					}
				} else {
					fputs("\\ud83d\\udca9", evout);
				}
			} else {
				switch (buf[j]) {
				case '"':
				case '\\':
					fputc('\\', evout); // output backslash for escaping
					fputc(buf[j], evout); // print the character itself
					break;
				default:
					fputc(buf[j], evout);
					break;
				}
			}
		}
	}

	fputs("\"]\n", evout);
}

void
outputproc(struct outargs *oa)
{
	unsigned char obuf[BUFSIZ];
	struct pollfd pollfds[2];
	int status;

	status = EXIT_SUCCESS;
	master = oa->masterfd;

	assert(oa->format_version == 1 || oa->format_version == 2 ||
	    oa->format_version == 3);

	if (oa->audioout || oa->devid) {
		assert(oa->audioout && oa->devid);
	}

	if (oa->audioout) {
		audio_enabled = 1;
		audio_init(oa->devid, oa->audioout, oa->use_raw);
	}

	start_paused = paused = oa->start_paused;

	evout = xfopen(oa->outfn, "wb");

	/* Write asciicast header and append events. Format defined at
	 * v1 https://github.com/asciinema/asciinema/blob/master/doc/asciicast-v1.md
	 * v2 https://github.com/asciinema/asciinema/blob/master/doc/asciicast-v2.md
	 * v3 https://docs.asciinema.org/manual/asciicast/v3/
	 *
	 * With v1, we insert an empty first record to avoid the hassle of dealing with
	 * ES (still) not supporting trailing commas.
	 */
	if (oa->format_version == 3) {
		fprintf(evout, "{\"version\": 3, \"term\": {\"cols\": %d, \"rows\": %d",
		    oa->cols, oa->rows);
		if (oa->term_type && *oa->term_type) {
			fprintf(evout, ", \"type\": \"%s\"", oa->term_type);
		}
		if (oa->term_version && *oa->term_version) {
			fprintf(evout, ", \"version\": \"%s\"", oa->term_version);
		}
		if (oa->theme_json && *oa->theme_json) {
			fprintf(evout, ", \"theme\": %s", oa->theme_json);
		}
		fprintf(evout, "}, \"timestamp\": %lld", (long long)oa->timestamp);
		if (oa->cmd && *oa->cmd) {
			fprintf(evout, ", \"command\": \"%s\"", oa->cmd);
		}
		if (oa->title && *oa->title) {
			fprintf(evout, ", \"title\": \"%s\"", oa->title);
		}
		if (oa->env && *oa->env) {
			fprintf(evout, ", \"env\": %s", oa->env);
		}
		fprintf(evout, "}\n");
	} else {
		fprintf(evout,
		    "{\"version\": %d, "
		    "\"width\": %d, "
		    "\"height\": %d, "
		    "\"command\": \"%s\", "
		    "\"title\": \"%s\", "
		    "\"env\": %s",
		    oa->format_version,
		    oa->cols, oa->rows,
		    oa->cmd ? oa->cmd : "",
		    oa->title ? oa->title : "",
		    oa->env
		);
		if (oa->format_version == 2 && oa->theme_json && *oa->theme_json) {
			fprintf(evout, ", \"theme\": %s", oa->theme_json);
		}
		if (oa->format_version == 2) {
			// v2 header finished here, data will be appended in separate lines
			fprintf(evout, "}\n");
		} else if (oa->format_version == 1) {
			// v1 header finished, console data is appended in structure
			fprintf(evout, ",\"stdout\":[[0,\"\"]\n");
		}
	}

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
			if (pollfds[i].revents & POLLIN) {
				if (pollfds[i].fd == oa->controlfd) {
					struct control_msg msg;
					ssize_t nread;

					nread = read(oa->controlfd, &msg, sizeof msg);
					if (nread == -1 || nread != sizeof msg) {
						perror("read");
						status = EXIT_FAILURE;
						goto end;
					}

					handle_command(&msg, oa->format_version);
				} else if (pollfds[i].fd == oa->masterfd) {
					ssize_t nread;

					nread = read(oa->masterfd, obuf, BUFSIZ);
					if (nread < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) {
							continue;
						}
						status = (errno == EIO) ?
						    EXIT_SUCCESS : EXIT_FAILURE;
						goto end;
					}
					if (nread == 0) {
						status = EXIT_SUCCESS;
						goto end;
					}

					xwrite(STDOUT_FILENO, obuf, nread);

					if (!paused) {
						handle_input(obuf, nread, oa->format_version);
					}
					if (nread == BUFSIZ) {
						continue;
					}
				}
			}

			if (pollfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
				status = (pollfds[i].revents & (POLLERR | POLLNVAL)) ?
				    EXIT_FAILURE : EXIT_SUCCESS;
				goto end;
			}
		}
	}

end:
	if (oa->format_version == 3 && oa->shellpid > 0) {
		int wstatus = 0;
		pid_t r = 0;

		for (int attempt = 0; attempt < 50; attempt++) {
			r = waitpid(oa->shellpid, &wstatus, WNOHANG);
			if (r != 0) {
				break;
			}
			struct timespec ts = { 0, 1000000L }; /* 1 ms */
			nanosleep(&ts, NULL);
		}
		if (r == oa->shellpid) {
			int code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) :
			    (WIFSIGNALED(wstatus) ? 128 + WTERMSIG(wstatus) : 1);
			double xdelta = (first || paused) ? 0.0 : next_delta_ms(0);
			fprintf(evout, "[%0.4f,\"x\",\"%d\"]\n",
			    xdelta / 1000, code);
		}
	} else if ((oa->format_version == 1 || oa->format_version == 2) &&
	    !first && !paused) {
		double edelta = next_delta_ms(0);
		if (oa->format_version == 2 && edelta >= 1.0) {
			fprintf(evout, "[%0.4f,\"o\",\"\"]\n", dur / 1000);
		}
	}
	if (oa->format_version == 1) {
		fprintf(evout, "], \"duration\": %.9g}\n", dur / 1000);
	}

	fflush(evout);

	if (oa->audioout && oa->devid) {
		if (!paused) {
			audio_stop();
		}

		audio_exit();
	}

	xfclose(evout);
	xclose(oa->masterfd);

	exit(status);
}

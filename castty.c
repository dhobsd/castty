/* See LICENSE for redistribution information */
#define _XOPEN_SOURCE 600

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include <poll.h>
#include <signal.h>
#include <termios.h>

#include "castty.h"

static char	*shell;
static FILE	*fscript;
static int	master;
static int	slave;
static int	child;
static int	subchild;
static char	*fname;

static struct	termios tt;
static struct	winsize win;
static int	aflg;
static char	*rflg;

int controlfd[2];

static FILE *
efopen(const char *path, const char *mode)
{
	FILE *fp = fopen(path, mode);
	if (fp == NULL) {
		perror("fopen");
		exit(EXIT_FAILURE);
	}
	return fp;
}

static int
edup2(int oldfd, int newfd)
{
	int fd = dup2(oldfd, newfd);
	if (fd == -1) {
		perror("dup2");
		exit(EXIT_FAILURE);
	}
	return fd;
}

static void
done(void)
{

	if (subchild) {
		(void) fclose(fscript);
		(void) close(master);
	} else {
		(void) tcsetattr(0, TCSAFLUSH, &tt);
		exit(EXIT_SUCCESS);
	}

}

static void
fail(void)
{

	(void) kill(0, SIGTERM);
	done();
}

static void
do_write(int fd, void *buf, size_t len)
{

	if (write(fd, buf, len) < 0) {
		perror("write");
		exit(EXIT_FAILURE);
	}
}

static void
doinput(int masterfd)
{
	unsigned char ibuf[BUFSIZ], *p;
	enum input_state {
		STATE_PASSTHROUGH,
		STATE_COMMAND,
	} input_state;
	ssize_t cc;

	input_state = STATE_PASSTHROUGH;

	while ((cc = read(STDIN_FILENO, ibuf, BUFSIZ)) > 0) {
		unsigned char *cmdstart;
		unsigned char *cmdend;

		p = ibuf;

		cmdstart = memchr(ibuf, 0x01, cc);
		if (cmdstart) {
			switch (input_state) {
			case STATE_PASSTHROUGH:
				/* Switching into command mode: pass through anything
				 * preceding our command
				 */
				if (cmdstart > ibuf) {
					do_write(masterfd, ibuf, cmdstart - ibuf);
				}
				cmdstart++;
				cc -= (cmdstart - ibuf);

				p = cmdstart;
				input_state = STATE_COMMAND;

				break;
			case STATE_COMMAND:
				break;
			}
		}

		switch (input_state) {
		case STATE_PASSTHROUGH:
			do_write(masterfd, ibuf, cc);
			break;

		case STATE_COMMAND:
			cmdend = memchr(p, '\r', cc);
			if (cmdend) {
				do_write(controlfd[1], p, cmdend - p + 1);
				cmdend++;

				if (cmdend - p + 1 < cc) {
					do_write(masterfd, p + 1, cc - (cmdend - p));
				}

				input_state = STATE_PASSTHROUGH;
			} else {
				do_write(controlfd[1], p, cc);
			}
			break;
		}

	}

	exit(EXIT_SUCCESS);
}

static void
finish(int sig)
{
	int status;
	int pid;
	int die = 0;

	(void)sig;

	while ((pid = waitpid(-1, (int *)&status, WNOHANG)) > 0) {
		if (pid == child) {
			die = 1;
		}
	}

	if (die) {
		done();
	}
}

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
	int off;
} command;

static void
handle_command(void)
{
	return;
}

static void
dooutput(void)
{
	static struct timeval prev, now;
	struct pollfd pollfds[2];
	static double dur;
	char obuf[BUFSIZ];
	int paused = 0;
	int first = 1;
	int cc;

	setbuf(stdout, NULL);
	(void) close(0);

	/* Clear screen */
	printf("\x1b[2J");

	/* Move cursor to top-left */
	printf("\x1b[H");

	fprintf(fscript, "var _ti={\"rows\":%d,\"cols\":%d};\n",
	    win.ws_row - 1, win.ws_col);
	fputs("var _tre=[{", fscript);

	int f = fcntl(master, F_GETFL);
	fcntl(master, F_SETFL, f | O_NONBLOCK);

	f = fcntl(controlfd[0], F_GETFL);
	fcntl(controlfd[0], F_SETFL, f | O_NONBLOCK);

	pollfds[0].fd = master;
	pollfds[0].events = POLLIN;
	pollfds[0].revents = 0;

	pollfds[1].fd = controlfd[0];
	pollfds[1].events = POLLIN;
	pollfds[1].revents = 0;

	for (;;) {
		int nready;

		nready = poll(pollfds, 2, -1);
		if (nready == -1) {
			perror("poll");
			exit(EXIT_FAILURE);
		}

		for (int i = 0; i < 2; i++) {
			if ((pollfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))) {
				goto end;
			}

			if (!(pollfds[i].revents & POLLIN)) {
				continue;
			}

			/* Control descriptor is highest priority */
			if (pollfds[i].fd == controlfd[0]) {
				unsigned char *epos;

				if (command.off == 0) {
					printf("\x1b" "7\x1b[0;37;1;40m");
				}

				cc = read(controlfd[0], command.cmdbuf + command.off, sizeof command.cmdbuf - command.off);

				epos = memchr(command.cmdbuf + command.off, '\r', cc);
				if (epos == NULL) {
					epos = memchr(command.cmdbuf + command.off, '\n', cc);
				}

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
			} else if (pollfds[i].fd == master) {
				cc = read(master, obuf, BUFSIZ);
				if (cc <= 0) {
					goto end;
				}

				do_write(STDOUT_FILENO, obuf, cc);

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

					fprintf(fscript, "\"s\":%0.3f,\"e\":\"", dur);

					for (int j = 0; j < cc; j++) {
						switch (obuf[j]) {
						case '"':
						case '\\':
							fputc('\\', fscript);
						default:
							if (!isprint(obuf[j])) {
								fprintf(fscript, "\\x%02hhx", obuf[j]);
							} else {
								fputc(obuf[j], fscript);
							}
							break;
						}
					}

					fputs("\"},{", fscript);
				}
			}
		}
	}

end:
	/* Empty last record */
	fprintf(fscript, "\"s\":%0.3f,\"e\":\"\"}];", dur);
	fflush(fscript);

	if (rflg) {
		audio_deinit();
	}

	done();
}

static void
doshell(const char *exec_cmd)
{

	(void) setsid();
	grantpt(master);
	unlockpt(master);
	if ((slave = open(ptsname(master), O_RDWR)) < 0) {
		perror("open");
		fail();
	}

	(void) setsid();
	(void) ioctl(slave, TIOCSCTTY, 0);
	(void) ioctl(slave, TIOCSWINSZ, (char *)&win);

	(void) close(master);
	(void) fclose(fscript);
	edup2(slave, 0);
	edup2(slave, 1);
	edup2(slave, 2);
	(void) close(slave);

	if (!exec_cmd) {
		execl(shell, strrchr(shell, '/') + 1, "-i", NULL);
	} else {
		execl(shell, strrchr(shell, '/') + 1, "-c", exec_cmd, NULL);
	}
	perror(shell);
	fail();
}

static void
fixtty(void)
{
	struct termios rtt;

	rtt = tt;
	rtt.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
	rtt.c_oflag &= ~OPOST;
	rtt.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	rtt.c_cflag &= ~(CSIZE|PARENB);
	rtt.c_cflag |= CS8;
	rtt.c_cc[VMIN] = 1;        /* read returns when one char is available.  */
	rtt.c_cc[VTIME] = 0;
	(void) tcsetattr(0, TCSAFLUSH, &rtt);
}

int
main(int argc, char **argv)
{
	char *exec_cmd = NULL;
	extern char *optarg;
	extern int optind;
	int ch;

	while ((ch = getopt(argc, argv, "aue:h?r:")) != EOF) {
		switch ((char)ch) {
		case 'a':
			aflg++;
			break;
		case 'e':
			exec_cmd = strdup(optarg);
			break;
		case 'r':
			rflg = strdup(optarg);
			break;
		case 'h':
		case '?':
		default:
			fprintf(stderr, "usage: ttyrec [-e command] [-a] [-r audio.raw] [file]\n");
			exit(EXIT_SUCCESS);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 0) {
		fname = argv[0];
	} else {
		fname = "events.js";
	}

	fscript = efopen(fname, aflg ? "ab" : "wb");
	setbuf(fscript, NULL);

	shell = getenv("SHELL");
	if (shell == NULL) {
		shell = "/bin/sh";
	}

	if (rflg) {
		audio_init(rflg, aflg);
	}

	if (pipe(controlfd) != 0) {
		perror("pipe");
		exit(EXIT_FAILURE);
	}

	(void) tcgetattr(0, &tt);
	master = posix_openpt(O_RDWR | O_NOCTTY);
	(void) ioctl(0, TIOCGWINSZ, (char *)&win);

	fixtty();

	signal(SIGCHLD, finish);

	child = fork();
	if (child < 0) {
		perror("fork");
		fail();
	}

	if (child == 0) {
		subchild = fork();
		if (subchild < 0) {
			perror("fork");
			fail();
		}

		if (subchild) {
			/* Handle output to file in parent */
			dooutput();
		} else {
			(void)close(controlfd[0]);
			(void)close(controlfd[1]);

			/* Run the shell in the child */
			doshell(exec_cmd);
		}
	}

	(void)close(controlfd[0]);
	(void)fclose(fscript);

	audio_toggle();
	doinput(master);

	return 0;
}

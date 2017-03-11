/* See LICENSE for redistribution information */
#define _XOPEN_SOURCE 600

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>

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
	}

	exit(EXIT_SUCCESS);
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
	unsigned char ibuf[BUFSIZ];
	ssize_t cc;

	while ((cc = read(STDIN_FILENO, ibuf, BUFSIZ)) > 0) {
		do_write(masterfd, ibuf, cc);
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

	fprintf(fscript, "var _ti={\"rows\":%hd,\"cols\":%hd};\n",
	    win.ws_row, win.ws_col);
	fputs("var _tre=[{", fscript);

	pollfds[0].fd = master;
	pollfds[0].events = (POLLIN | POLLERR | POLLHUP);

	pollfds[1].fd = controlfd[0];
	pollfds[1].events = (POLLIN | POLLERR | POLLHUP);

	for (;;) {
		int nready;

		nready = poll(pollfds, 2, -1);
		if (nready == -1) {
			perror("poll");
			exit(EXIT_FAILURE);
		}

		for (int i = 0; i < nready; i++) {
			if ((pollfds[i].revents & (POLLHUP | POLLERR))) {
				goto end;
			}

			/* Control descriptor is highest priority */
			if (pollfds[i].fd == controlfd[0]) {
				unsigned char code;

				cc = read(controlfd[0], &code, 1);
				if (cc != 1) {
					perror("read");
					exit(EXIT_FAILURE);
				}

				switch (code) {
				case 0:
					audio_toggle();
					paused = !paused;

					/* If we're unpausing, try not to add anything much */
					if (!paused) {
						gettimeofday(&now, NULL);
						prev = now;
					}

					break;
				default:
					fprintf(stdout, "Invalid code: %02x\r\n", code);
					exit(EXIT_FAILURE);
				}
			} else if (pollfds[i].fd == master) {
				cc = read(master, obuf, BUFSIZ);
				if (cc <= 0) {
					break;
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

	done();
}

static void
doshell(const char* command)
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

	if (!command) {
		execl(shell, strrchr(shell, '/') + 1, "-i", NULL);
	} else {
		execl(shell, strrchr(shell, '/') + 1, "-c", command, NULL);
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
	char *command = NULL;
	extern char *optarg;
	extern int optind;
	int ch;

	while ((ch = getopt(argc, argv, "aue:h?r:")) != EOF) {
		switch ((char)ch) {
		case 'a':
			aflg++;
			break;
		case 'e':
			command = strdup(optarg);
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

	(void) tcgetattr(0, &tt);
	master = posix_openpt(O_RDWR | O_NOCTTY);
	(void) ioctl(0, TIOCGWINSZ, (char *)&win);

	fixtty();

	(void) signal(SIGCHLD, finish);

	if (pipe(controlfd) != 0) {
		perror("pipe");
		exit(EXIT_FAILURE);
	}

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
			(void) close(controlfd[0]);
			(void) close(controlfd[1]);

			/* Run the shell in the child */
			doshell(command);
		}
	}

	/* Don't output in parent process */
	(void)fclose(fscript);

	audio_toggle();
	doinput(master);

	return 0;
}

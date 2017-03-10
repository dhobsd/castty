/* See LICENSE for redistribution information */
#define _XOPEN_SOURCE 600

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

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

static void
done(void)
{

	if (subchild) {
		Header h;

		gettimeofday(&h.tv, NULL);
		fflush(fscript);
		(void) write_header(fscript, &h);
		fputs("\"}];", fscript);

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
	unsigned char ibuf[BUFSIZ];
	static int paused = 0;
	ssize_t cc;

	/* Read input from stdin and write it to our master pty handle */
	while ((cc = read(STDIN_FILENO, ibuf, BUFSIZ)) > 0) {
		unsigned char *pause;

		pause = memchr(ibuf, 0x05, cc);
		if (pause != NULL) {
			if (!paused) {
				if (pause > ibuf) {
					do_write(masterfd, ibuf, pause - ibuf - 1);
				}
				fprintf(stderr, "\r\n*** PAUSED ***\r\n");
			} else {
				ssize_t diff = pause - ibuf;
				if (diff < cc) {
					diff = cc - diff;
					do_write(masterfd, pause + 1, diff);
				}
				fprintf(stderr, "\r\n*** UNPAUSED ***\r\n");
			}

			audio_toggle();
			paused = !paused;
		}

		if (!paused) {
			do_write(masterfd, ibuf, cc);
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

static void
dooutput(void)
{
	int cc;
	char obuf[BUFSIZ];

	setbuf(stdout, NULL);
	(void) close(0);

	fprintf(fscript, "var _ti={\"rows\":%hd,\"cols\":%hd};\n",
	    win.ws_row, win.ws_col);

	for (;;) {
		Header h;

		cc = read(master, obuf, BUFSIZ);
		if (cc <= 0) {
			break;
		}

		h.len = cc;

		gettimeofday(&h.tv, NULL);

		if (write(1, obuf, cc) < 0) {
			perror("write");
			done();
		}

		write_header(fscript, &h);
		for (int i = 0; i < cc; i++) {
			switch (obuf[i]) {
			case '"':
			case '\\':
				fputc('\\', fscript);
			default:
				if (!isprint(obuf[i])) {
					fprintf(fscript, "\\x%02hhx", obuf[i]);
				} else {
					fputc(obuf[i], fscript);
				}
				break;
			}
		}
	}

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
	(void) dup2(slave, 0);
	(void) dup2(slave, 1);
	(void) dup2(slave, 2);
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
			exit(1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 0) {
		fname = argv[0];
	} else {
		fname = "events.js";
	}

	if (rflg) {
		audio_init(rflg, aflg);
	}

	if ((fscript = fopen(fname, aflg ? "ab" : "wb")) == NULL) {
		perror(fname);
		fail();
	}
	setbuf(fscript, NULL);

	shell = getenv("SHELL");
	if (shell == NULL) {
		shell = "/bin/sh";
	}

	(void) tcgetattr(0, &tt);
	master = posix_openpt(O_RDWR | O_NOCTTY);
	(void) ioctl(0, TIOCGWINSZ, (char *)&win);

	fixtty();

	(void) signal(SIGCHLD, finish);

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

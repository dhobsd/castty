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

#include "audio.h"
#include "ttyrec.h"
#include "io.h"

void done(void);
void fail(void);
void fixtty(void);
void getslave(void);
void doinput(void);
void dooutput(void);
void doshell(const char*);

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

int
main(int argc, char **argv)
{
	extern int optind;
	int ch;
	void finish();
	char *getenv();
	char *command = NULL;

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
			fprintf(stderr, "usage: ttyrec [-e command] [-a] [file]\n");
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
			dooutput();
		} else {
			doshell(command);
		}
	}
	audio_toggle();
	doinput();

	return 0;
}

void
doinput(void)
{
	char ibuf[BUFSIZ];
	int cc;

	(void) fclose(fscript);

	while ((cc = read(0, ibuf, BUFSIZ)) > 0) {
		(void) write(master, ibuf, cc);
	}
	done();
}

void
finish(void)
{
	int status;
	int pid;
	int die = 0;

	while ((pid = waitpid(-1, (int *)&status, WNOHANG)) > 0) {
		if (pid == child) {
			die = 1;
		}
	}

	if (die) {
		done();
	}
}

void
dooutput()
{
	int cc;
	char obuf[BUFSIZ];

	setbuf(stdout, NULL);
	(void) close(0);

	for (;;) {
		Header h;

		cc = read(master, obuf, BUFSIZ);
		if (cc <= 0) {
			break;
		}

		h.len = cc;

		gettimeofday(&h.tv, NULL);
		(void) write(1, obuf, cc);
		(void) write_header(fscript, &h);
		for (size_t i = 0; i < cc; i++) {
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

void
doshell(const char* command)
{
	getslave();
	(void) close(master);
	(void) fclose(fscript);
	(void) dup2(slave, 0);
	(void) dup2(slave, 1);
	(void) dup2(slave, 2);
	(void) close(slave);

	if (!command) {
		execl(shell, strrchr(shell, '/') + 1, "-i", 0);
	} else {
		execl(shell, strrchr(shell, '/') + 1, "-c", command, 0);	
	}
	perror(shell);
	fail();
}

void
fixtty()
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

void
fail()
{

	(void) kill(0, SIGTERM);
	done();
}

void
done()
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

		exit(0);
	}
}


void
getslave()
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
}

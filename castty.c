/* See LICENSE for redistribution information */
#define _XOPEN_SOURCE 600

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "castty.h"

static pid_t child, subchild;
static struct termios tt;

static void
handle_sigchld(int sig)
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
		xtcsetattr(0, TCSAFLUSH, &tt);
		exit(EXIT_SUCCESS);
	}
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

	xtcsetattr(0, TCSAFLUSH, &rtt);
}

int
main(int argc, char **argv)
{
	int ch, aflg, masterfd, controlfd[2];
	char *exec_cmd, *rflg, *outfile;
	extern char *optarg;
	struct winsize win;
	extern int optind;

	aflg = 0;
	exec_cmd = rflg = NULL;

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
		outfile = argv[0];
	} else {
		outfile = "events.js";
	}

	if (rflg) {
		audio_init(rflg, aflg);
	}

	if (pipe(controlfd) != 0) {
		perror("pipe");
		exit(EXIT_FAILURE);
	}

	if (tcgetattr(0, &tt) == -1) {
		perror("tcgetattr");
		exit(EXIT_FAILURE);
	}

	masterfd = posix_openpt(O_RDWR | O_NOCTTY);
	if (masterfd == -1) {
		perror("posix_openpt");
		exit(EXIT_FAILURE);
	}

	if (ioctl(0, TIOCGWINSZ, &win) == -1) {
		perror("ioctl(TIOCGWINSZ)");
		exit(EXIT_FAILURE);
	}

	fixtty();

	signal(SIGCHLD, handle_sigchld);

	child = fork();
	if (child < 0) {
		perror("fork");
		exit(EXIT_FAILURE);
	}

	if (child == 0) {
		subchild = fork();
		if (subchild < 0) {
			perror("fork");
			if (kill(0, SIGTERM) == -1) {
				perror("kill");
			}

			exit(EXIT_FAILURE);
		}

		if (subchild) {
			/* Handle output to file in parent */
			xclose(controlfd[1]);
			outputproc(masterfd, controlfd[0], outfile, aflg, win.ws_row,
			    win.ws_col);
		} else {
			char *shell = getenv("SHELL");
			if (shell == NULL) {
				shell = "/bin/sh";
			}

			/* Shell process doesn't need these */
			xclose(controlfd[0]);
			xclose(controlfd[1]);

			/* Run the shell in the child */
			shellproc(shell, exec_cmd, &win, &tt, masterfd);
		}
	}

	xclose(controlfd[0]);

	if (rflg) {
		audio_toggle();
	}

	inputproc(masterfd, controlfd[1]);

	if (rflg) {
		audio_deinit();
	}

	return 0;
}

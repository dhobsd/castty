#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <sys/wait.h>

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "castty.h"

static struct winsize owin, rwin, win;
static pid_t child, subchild;
static struct termios tt;
static int masterfd;

static void
handle_sigchld(int sig)
{
	int status;
	pid_t pid;

	(void)sig;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (pid == child || pid == subchild) {
			close(STDIN_FILENO);
		}
	}
}

static void
handle_sigwinch(int sig)
{

	(void)sig;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &rwin) == -1) {
		perror("ioctl(TIOCGWINSZ)");
		exit(EXIT_FAILURE);
	}

	if (rwin.ws_col <= owin.ws_col || rwin.ws_row < owin.ws_row) {
		win = rwin;
		if (ioctl(masterfd, TIOCSWINSZ, &win) == -1) {
			perror("ioctl(TIOCSWINSZ)");
			exit(EXIT_FAILURE);
		}
	}
}

static void
set_raw_input(void)
{
	struct termios rtt;

	rtt = tt;
	rtt.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|INLCR|IGNCR|ICRNL|IXON);
	rtt.c_oflag &= ~OPOST;
	rtt.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	rtt.c_cflag &= ~(CSIZE|PARENB);
	rtt.c_cflag |= CS8;
	rtt.c_cc[VMIN] = 1;
	rtt.c_cc[VTIME] = 0;

	xtcsetattr(STDIN_FILENO, TCSAFLUSH, &rtt);
}

int
main(int argc, char **argv)
{
	char *exec_cmd, *audioout, *outfile, *devid;
	int ch, controlfd[2];
	extern char *optarg;
	extern int optind;
	long rows, cols;

	exec_cmd = audioout = devid = NULL;
	rows = cols = 0;

	while ((ch = getopt(argc, argv, "?a:c:d:e:hlmr:")) != EOF) {
		char *e;

		switch (ch) {
		case 'a':
			audioout = strdup(optarg);
			break;
		case 'c':
			errno = 0;
			cols = strtol(optarg, &e, 10);
			if (e == optarg || errno != 0 || cols > 1000) {
				fprintf(stderr, "castty: Invalid column count: %ld\n",
				    cols);
				exit(EXIT_FAILURE);
			}
			break;
		case 'd':
			devid = strdup(optarg);
			break;
		case 'e':
			exec_cmd = strdup(optarg);
			break;
		case 'l':
			audio_list();
			exit(EXIT_SUCCESS);
			break;
		case 'm':
			audio_toggle_mp3();
			break;
		case 'r':
			errno = 0;
			rows = strtol(optarg, &e, 10);
			if (e == optarg || errno != 0 || rows > 1000) {
				fprintf(stderr, "castty: Invalid row count: %ld\n",
				    rows);
				exit(EXIT_FAILURE);
			}
			break;

		case 'h':
		case '?':
		default:
			fprintf(stderr, "usage: castty [-adlcre] [out.js]\n"
			    " -a <outfile>   Output audio to <outfile>. Must be specified with -d.\n"
			    " -c <cols>      Use <cols> columns in the recorded shell session.\n"
			    " -d <device>    Use audio device <device> for input.\n"
			    " -e <cmd>       Execute <cmd> from the recorded shell session.\n"
			    " -l             List available audio input devices and exit.\n"
			    " -m             Encode audio to mp3 before writing.\n"
			    " -r <rows>      Use <rows> rows in the recorded shell session.\n"
			    "\n"
			    " [out.js]       Optional output filename of recorded events. If not specified,\n"
			    "                a file \"events.js\" will be created.\n");
			exit(EXIT_SUCCESS);
		}
	}

	if ((audioout == NULL && devid != NULL) ||
	    (devid == NULL && audioout != NULL)) {
		fprintf(stderr, "If -d or -a are specified, both must appear.\n");
		exit(EXIT_FAILURE);
	}

	argc -= optind;
	argv += optind;

	if (argc > 0) {
		outfile = argv[0];
	} else {
		outfile = "events.js";
	}

	if (pipe(controlfd) != 0) {
		perror("pipe");
		exit(EXIT_FAILURE);
	}

	if (tcgetattr(STDIN_FILENO, &tt) == -1) {
		perror("tcgetattr");
		exit(EXIT_FAILURE);
	}

	masterfd = posix_openpt(O_RDWR | O_NOCTTY);
	if (masterfd == -1) {
		perror("posix_openpt");
		exit(EXIT_FAILURE);
	}

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &win) == -1) {
		perror("ioctl(TIOCGWINSZ)");
		exit(EXIT_FAILURE);
	}

	rwin = owin = win;

	if (!rows || rows > win.ws_row) {
		rows = win.ws_row;
	}

	if (!cols || cols > win.ws_col) {
		cols = win.ws_col;
	}

	win.ws_row = rows;
	win.ws_col = cols;

	set_raw_input();

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
			outputproc(masterfd, controlfd[0], outfile, audioout,
			    devid, 0, win.ws_row, win.ws_col);
		} else {
			char *shell = getenv("SHELL");
			if (shell == NULL) {
				shell = "/bin/sh";
			}

			/* Shell process doesn't need these */
			xclose(controlfd[0]);
			xclose(controlfd[1]);

			/* Run the shell in the child */
			shellproc(shell, exec_cmd, &win, masterfd);
		}
	} else {
		signal(SIGWINCH, handle_sigwinch);
	}

	xclose(controlfd[0]);
	inputproc(masterfd, controlfd[1]);

	signal(SIGWINCH, NULL);

	if (ioctl(STDOUT_FILENO, TIOCSWINSZ, &rwin) == -1) {
		perror("ioctl(TIOCSWINSZ)");
	}

	if (tcsetattr(STDOUT_FILENO, TCSAFLUSH, &tt) == -1) {
		perror("tcsetattr");
	}

	return 0;
}

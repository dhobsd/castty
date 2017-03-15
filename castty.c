#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <sys/wait.h>

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "castty.h"

extern char **environ;

static struct winsize owin, rwin, win;
static pid_t child, subchild;
static struct termios tt;
static int masterfd;

static void
do_backtrace(int sig)
{
	void *callstack[128];
	char **strs;
	int frames;

	frames = backtrace(callstack, 128);
	strs = backtrace_symbols(callstack, frames);

	for (int i = 0; i < frames; ++i) {
		fprintf(stderr, "\n\r%s", strs[i]);
	}

	fprintf(stderr, "\n\r");
	free(strs);
	exit(EXIT_FAILURE);
}

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

static char *
escape(const char *from)
{
	unsigned i, o;
	char *p;

	p = malloc(strlen(from) * 2 + 1);
	if (p == NULL) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}

	for (i = 0, o = 0; i < strlen(from); i++) {
		switch (from[i]) {
		case '\\':
		case '"':
			p[o++] = '\\';
		default:
			p[o++] = from[i];
		}
	}

	p[o] = '\0';

	return p;
}

static char *
serialize_env(void)
{
	size_t o = 0, s = 1024;
	char *p;

	p = malloc(s);
	if (p == NULL) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}

	p[o++] = '{';

	for (unsigned i = 0; environ[i] != NULL; i++) {
		char *e, *k, *v;
		size_t l;

		e = escape(environ[i]);
		l = strlen(e);

		k = e;
		v = strchr(e, '=');
		if (v == NULL) {
			fprintf(stderr, "Your environment is whack.\n");
			exit(EXIT_FAILURE);
		}

		*v = '\0';
		v++;

		/* 7 is for two sets of quotes, a colon, a comma, and a 0 byte */
		if (o + l + 7 >= s) {
			size_t ns;
			char *r;

			ns = s + l + 1025;
			r = realloc(p, ns);
			if (r == NULL) {
				perror("realloc");
				exit(EXIT_FAILURE);
			}

			s = ns;
			p = r;
		}

		p[o++] = '"';
		memcpy(p + o, k, strlen(k));
		o += strlen(k);
		p[o++] = '"';
		p[o++] = ':';
		p[o++] = '"';
		memcpy(p + o, v, strlen(v));
		o += strlen(v);
		p[o++] = '"';
		if (environ[i + 1]) {
			p[o++] = ',';
		}

		free(e);
	}

	p[o++] = '}';

	return p;
}

int
main(int argc, char **argv)
{
	int ch, controlfd[2];
	extern char *optarg;
	extern int optind;
	struct outargs oa;
	char *exec_cmd;

	memset(&oa, 0, sizeof oa);
	oa.env = serialize_env();
	exec_cmd = NULL;

	while ((ch = getopt(argc, argv, "?a:c:d:e:hlmpr:t:")) != EOF) {
		char *e;

		switch (ch) {
		case 'a':
			oa.audioout = strdup(optarg);
			break;
		case 'c':
			errno = 0;
			oa.cols = strtol(optarg, &e, 10);
			if (e == optarg || errno != 0 || oa.cols > 1000) {
				fprintf(stderr, "castty: Invalid column count: %d\n",
				    oa.cols);
				exit(EXIT_FAILURE);
			}
			break;
		case 'd':
			oa.devid = strdup(optarg);
			break;
		case 'e':
			exec_cmd = strdup(optarg);
			oa.cmd = escape(exec_cmd);
			break;
		case 'l':
			audio_list();
			exit(EXIT_SUCCESS);
			break;
		case 'm':
			audio_toggle_mp3();
			break;
		case 'p':
			oa.start_paused = 1;
			break;
		case 'r':
			errno = 0;
			oa.rows = strtol(optarg, &e, 10);
			if (e == optarg || errno != 0 || oa.rows > 1000) {
				fprintf(stderr, "castty: Invalid row count: %d\n",
				    oa.rows);
				exit(EXIT_FAILURE);
			}
			break;
		case 't':
			oa.title = escape(optarg);
			break;
		case 'h':
		case '?':
		default:
			fprintf(stderr, "usage: castty [-acdelrt] [out.json]\n"
			    " -a <outfile>   Output audio to <outfile>. Must be specified with -d.\n"
			    " -c <cols>      Use <cols> columns in the recorded shell session.\n"
			    " -d <device>    Use audio device <device> for input.\n"
			    " -e <cmd>       Execute <cmd> from the recorded shell session.\n"
			    " -l             List available audio input devices and exit.\n"
			    " -m             Encode audio to mp3 before writing.\n"
			    " -p             Begin the recording in paused mode.\n"
			    " -r <rows>      Use <rows> rows in the recorded shell session.\n"
			    " -t <title>     Title of the cast.\n"
			    "\n"
			    " [out.json]     Optional output filename of recorded events. If not specified,\n"
			    "                a file \"events.json\" will be created.\n");
			exit(EXIT_SUCCESS);
		}
	}

	if ((oa.audioout == NULL && oa.devid != NULL) ||
	    (oa.devid == NULL && oa.audioout != NULL)) {
		fprintf(stderr, "If -d or -a are specified, both must appear.\n");
		exit(EXIT_FAILURE);
	}

	argc -= optind;
	argv += optind;

	if (argc > 0) {
		oa.outfn = argv[0];
	} else {
		oa.outfn = "events.json";
	}

	if (pipe(controlfd) != 0) {
		perror("pipe");
		exit(EXIT_FAILURE);
	}
	oa.controlfd = controlfd[0];

	if (tcgetattr(STDIN_FILENO, &tt) == -1) {
		perror("tcgetattr");
		exit(EXIT_FAILURE);
	}

	oa.masterfd = masterfd = posix_openpt(O_RDWR | O_NOCTTY);
	if (masterfd == -1) {
		perror("posix_openpt");
		exit(EXIT_FAILURE);
	}

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &win) == -1) {
		perror("ioctl(TIOCGWINSZ)");
		exit(EXIT_FAILURE);
	}

	rwin = owin = win;

	if (!oa.rows || oa.rows > win.ws_row) {
		oa.rows = win.ws_row;
	}

	if (!oa.cols || oa.cols > win.ws_col) {
		oa.cols = win.ws_col;
	}

	win.ws_row = oa.rows;
	win.ws_col = oa.cols;

	set_raw_input();

	signal(SIGCHLD, handle_sigchld);
	signal(SIGSEGV, do_backtrace);
	signal(SIGBUS, do_backtrace);

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
			outputproc(&oa);
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

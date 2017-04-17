#include <sys/ioctl.h>

#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "audio/writer-lame.h"
#include "audio.h"
#include "castty.h"
#include "record.h"

extern char **environ;

struct winsize owin, rwin, win;
struct termios tt;
FILE *debug_out;
int masterfd;
pid_t child;

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

	assert(from);

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
	static const char *interesting[] = {
		"TERM",
		"SHELL",
		"PS1",
		"PS2",
	};
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
		int useful, rv;
		size_t l;

		e = escape(environ[i]);

		k = e;
		v = strchr(e, '=');
		if (v == NULL) {
			fprintf(stderr, "Your environment is whack.\n");
			exit(EXIT_FAILURE);
		}

		*v++ = '\0';

		useful = 0;
		for (unsigned j = 0; j < sizeof interesting / sizeof interesting[0]; j++) {
			if (strcmp(k, interesting[j]) == 0) {
				useful = 1;
				break;
			}
		}

		if (!useful) {
			goto not_useful;
		}

		l = strlen(k) + strlen(v);
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

		rv = snprintf(&p[o], s - o, "\"%s\":\"%s\",", k, v);
		assert(rv > 0);
		assert((unsigned)rv < s - o);
		o += rv;

not_useful:	free(e);
	}

	if (p[o - 1] == ',') {
		p[o - 1] = '}';
	} else {
		p[o] = '}';
	}

	return p;
}

static void
usage(int status)
{

	fprintf(stderr, "usage: castty record [-acDdehl" LAME_OPT "prt] [out.json]\n"
	    " -a <outfile>   Output audio to <outfile>. Must be specified with -d.\n"
	    " -c <cols>      Use <cols> columns in the recorded shell session.\n"
	    " -D <outfile>   Send debugging information into <outfile>.\n"
	    " -d <device>    Use audio device <device> for input.\n"
	    " -e <cmd>       Execute <cmd> from the recorded shell session.\n"
	    " -h             Show this help.\n"
	    " -l             List available audio input devices and exit.\n"
#ifdef WITH_LAME
	    " -m             Encode audio to mp3 before writing.\n"
#endif
	    " -p             Begin the recording in paused mode.\n"
	    " -r <rows>      Use <rows> rows in the recorded shell session.\n"
	    " -t <title>     Title of the cast.\n"
	    "\n"
	    " [out.json]     Optional output filename of recorded events. If not specified,\n"
	    "                a file \"events.json\" will be created.\n");
	
	exit(status);
}

int
record_main(int argc, char **argv)
{
	int ch, controlfd[2];
	extern char *optarg;
	extern int optind;
	struct outargs oa;
	char *exec_cmd;

	memset(&oa, 0, sizeof oa);
	oa.env = serialize_env();
	exec_cmd = NULL;

	while ((ch = getopt(argc, argv, "?a:c:D:d:e:hlpr:t:" LAME_OPT)) != EOF) {
		char *e;

		switch (ch) {
		case 'a':
			oa.audioout = strdup(optarg);
			break;
		case 'c':
			errno = 0;
			oa.cols = strtol(optarg, &e, 10);
			if (e == optarg || errno != 0 || oa.cols > 1000) {
				fprintf(stderr, "castty: Invalid column count: %s\n",
				    optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 'D':
			debug_out = xfopen(optarg, "w");
			break;
		case 'd':
			oa.devid = strdup(optarg);
			break;
		case 'e':
			exec_cmd = strdup(optarg);
			oa.cmd = escape(exec_cmd);
			break;
		case 'l':
			audio_list_inputs();
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
				fprintf(stderr, "castty: Invalid row count: %s\n",
				    optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 't':
			oa.title = escape(optarg);
			break;
		case 'h':
		case '?':
			usage(EXIT_SUCCESS);
			break;
		default:
			usage(EXIT_FAILURE);
			break;
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

	xtcgetattr(STDIN_FILENO, &tt);

	oa.masterfd = masterfd = posix_openpt(O_RDWR | O_NOCTTY);
	if (masterfd == -1) {
		perror("posix_openpt");
		exit(EXIT_FAILURE);
	}

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &win) == -1) {
		perror("ioctl(TIOCGWINSZ)");
		exit(EXIT_FAILURE);
	}

	owin = rwin = win;

	if (!oa.rows || oa.rows > win.ws_row) {
		oa.rows = win.ws_row;
	}

	if (!oa.cols || oa.cols > win.ws_col) {
		oa.cols = win.ws_col;
	}

	win.ws_row = oa.rows;
	win.ws_col = oa.cols;

	set_raw_input();

	child = fork();
	if (child < 0) {
		perror("fork");
		exit(EXIT_FAILURE);
	}

	if (child == 0) {
		pid_t subchild = fork();
		if (subchild < 0) {
			perror("fork");
			if (kill(0, SIGTERM) == -1) {
				perror("kill");
			}

			exit(EXIT_FAILURE);
		}

		signal(SIGWINCH, SIG_IGN);

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
	}

	xclose(controlfd[0]);
	inputproc(masterfd, controlfd[1]);

	signal(SIGWINCH, NULL);

	if (ioctl(STDIN_FILENO, TIOCSWINSZ, &rwin) == -1) {
		perror("ioctl(TIOCSWINSZ)");
	}

	xtcsetattr(STDIN_FILENO, TCSAFLUSH, &tt);

	return 0;
}

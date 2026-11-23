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
#include "cast.h"
#include "castty.h"
#include "json.h"
#include "record.h"
#include "term.h"

extern char **environ;

struct winsize owin, rwin, win;
struct termios tt;
FILE *debug_out;
int masterfd;
pid_t child;
volatile sig_atomic_t ctrl_write_fd = -1;
volatile sig_atomic_t sigchld_write_fd = -1;
volatile sig_atomic_t tty_saved = 0;

void
restore_tty(void)
{
	if (!tty_saved) {
		return;
	}
	tty_saved = 0;

	(void)ioctl(STDIN_FILENO, TIOCSWINSZ, &rwin);
	(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &tt);
	term_write_reset(STDOUT_FILENO);
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
serialize_env(void)
{
	static const char *interesting[] = {
		"TERM",
		"TERM_PROGRAM",
		"COLORTERM",
		"COLORFGBG",
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
		const char *eq, *k;
		char *v;
		size_t klen, l;
		int rv;

		eq = strchr(environ[i], '=');
		if (eq == NULL) {
			fprintf(stderr, "Your environment is whack.\n");
			exit(EXIT_FAILURE);
		}

		klen = (size_t)(eq - environ[i]);
		k = NULL;
		for (unsigned j = 0; j < sizeof interesting / sizeof interesting[0]; j++) {
			if (strlen(interesting[j]) == klen &&
			    strncmp(environ[i], interesting[j], klen) == 0) {
				k = interesting[j];
				break;
			}
		}

		if (k == NULL) {
			continue;
		}

		v = json_escape(eq + 1);

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

		free(v);
	}

	if (o > 1 && p[o - 1] == ',') {
		p[o - 1] = '}';
	} else {
		p[o++] = '}';
		p[o] = '\0';
	}

	return p;
}

static void
usage(int status)
{

	fprintf(stderr, "usage: castty record [-123AacDdFehlmNprtR] [out.cast]\n"
	    " -1             Output asciicast v1 format.\n"
	    " -2             Output asciicast v2 format.\n"
	    " -3             Output asciicast v3 format (default).\n"
	    " -A             Append to an existing recording instead of overwriting.\n"
	    " -a <outfile>   Output audio to <outfile>. Must be specified with -d.\n"
	    " -c <cols>      Use <cols> columns in the recorded shell session.\n"
	    " -D <outfile>   Send debugging information into <outfile>.\n"
	    " -d <device>    Use audio device <device> for input.\n"
	    " -e <cmd>       Execute <cmd> from the recorded shell session.\n"
	    " -F <format>    Audio output format: raw, mp3, wav, flac, opus, ogg.\n"
	    " -h             Show this help.\n"
	    " -l             List available audio input devices and exit.\n"
	    " -m             Encode audio to mp3 before writing (shorthand for -F mp3).\n"
	    " -N             Disable terminal color/version probing.\n"
	    " -p             Begin the recording in paused mode.\n"
	    " -r <rows>      Use <rows> rows in the recorded shell session.\n"
	    " -R             Use a raw sound device.\n"
	    " -t <title>     Title of the cast.\n"
	    "\n"
	    " [out.cast]     Optional output filename of recorded events. If not specified,\n"
	    "                a file \"events.cast\" will be created.\n");
	exit(status);
}

int
record_main(int argc, char **argv)
{
	int ch, controlfd[2], sigchld_pipe[2], no_probe;
	extern char *optarg;
	extern int optind;
	struct outargs oa;
	struct term_probe probe;
	char *exec_cmd, *term;

	memset(&oa, 0, sizeof oa);
	memset(&probe, 0, sizeof probe);
	oa.env = serialize_env();
	term = getenv("TERM");
	if (term && *term) {
		oa.term_type = json_escape(term);
	}
	oa.timestamp = time(NULL);
	exec_cmd = NULL;
	no_probe = 0;

	while ((ch = getopt(argc, argv, "?Aa:c:D:d:e:F:hlmNpr:Rt:123")) != EOF) {
		char *e;

		switch (ch) {
		case 'A':
			oa.append = 1;
			break;
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
			oa.cmd = json_escape(exec_cmd);
			break;
		case 'F':
			if (audio_set_format(optarg) != 0) {
				fprintf(stderr,
				    "castty: unsupported audio format: %s\n",
				    optarg);
				usage(EXIT_FAILURE);
			}
			break;
		case 'l':
			audio_list_inputs();
			exit(EXIT_SUCCESS);
			break;
		case 'm':
			audio_toggle_mp3();
			break;
		case 'N':
			no_probe = 1;
			break;
		case 'p':
			oa.start_paused = 1;
			break;
		case '1':
			oa.format_version = 1;
			break;
		case '2':
			oa.format_version = 2;
			break;
		case '3':
			oa.format_version = 3;
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
		case 'R':
			oa.use_raw = 1;
			break;
		case 't':
			oa.title = json_escape(optarg);
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
		oa.outfn = "events.cast";
	}

	if (oa.append) {
		char errbuf[256];

		if (cast_inspect(oa.outfn, oa.format_version, &oa.append_info,
		    errbuf, sizeof errbuf) != 0) {
			fprintf(stderr, "%s\n", errbuf);
			exit(EXIT_FAILURE);
		}
		if (oa.append_info.exists) {
			oa.format_version = oa.append_info.version;
		}
		if (oa.audioout != NULL &&
		    audio_check_append(oa.audioout, errbuf, sizeof errbuf) != 0) {
			fprintf(stderr, "%s\n", errbuf);
			cast_append_info_free(&oa.append_info);
			exit(EXIT_FAILURE);
		}
	}

	if (!oa.format_version) {
		oa.format_version = 3;
	}

	if (pipe(controlfd) != 0) {
		perror("pipe");
		exit(EXIT_FAILURE);
	}
	oa.controlfd = controlfd[0];

	if (pipe(sigchld_pipe) != 0) {
		perror("pipe");
		exit(EXIT_FAILURE);
	}
	if (fcntl(sigchld_pipe[0], F_SETFD, FD_CLOEXEC) != 0 ||
	    fcntl(sigchld_pipe[1], F_SETFD, FD_CLOEXEC) != 0 ||
	    fcntl(sigchld_pipe[0], F_SETFL, O_NONBLOCK) != 0 ||
	    fcntl(sigchld_pipe[1], F_SETFL, O_NONBLOCK) != 0) {
		perror("fcntl");
		exit(EXIT_FAILURE);
	}

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
	term_reset_init(term, STDOUT_FILENO);
	tty_saved = 1;

	if (!oa.rows || oa.rows > win.ws_row) {
		oa.rows = win.ws_row;
	}

	if (!oa.cols || oa.cols > win.ws_col) {
		oa.cols = win.ws_col;
	}

	win.ws_row = oa.rows;
	win.ws_col = oa.cols;

	set_raw_input();

	if (!no_probe && oa.format_version >= 2 &&
	    !(oa.append && oa.append_info.exists)) {
		term_probe_tty(STDIN_FILENO, STDOUT_FILENO, term, &probe);
		oa.term_version = probe.version;
		oa.theme_json = probe.theme_json;
	}

	sigchld_write_fd = sigchld_pipe[1];
	child = fork();
	if (child < 0) {
		sigchld_write_fd = -1;
		perror("fork");
		exit(EXIT_FAILURE);
	}

	if (child == 0) {
		struct sigaction dfl;

		sigchld_write_fd = -1;
		tty_saved = 0;
		xclose(sigchld_pipe[0]);
		xclose(sigchld_pipe[1]);

		memset(&dfl, 0, sizeof dfl);
		(void)sigemptyset(&dfl.sa_mask);
		dfl.sa_handler = SIG_DFL;
		xsigaction(SIGCHLD, &dfl, NULL);
		xsigaction(SIGTERM, &dfl, NULL);
		xsigaction(SIGHUP, &dfl, NULL);

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
			oa.shellpid = subchild;
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
	ctrl_write_fd = controlfd[1];
	inputproc(masterfd, controlfd[1], sigchld_pipe[0]);
	ctrl_write_fd = -1;
	sigchld_write_fd = -1;
	xclose(sigchld_pipe[0]);
	xclose(sigchld_pipe[1]);
	xclose(controlfd[1]);

	signal(SIGWINCH, SIG_DFL);

	restore_tty();
	term_probe_free(&probe);
	cast_append_info_free(&oa.append_info);

	return 0;
}

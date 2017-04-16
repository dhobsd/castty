#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/signal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "castty.h"

extern struct winsize owin, rwin, win;
extern struct termios tt;
extern FILE *debug_out;
extern int masterfd;
extern pid_t child;

static void
do_backtrace(int sig, siginfo_t *siginfo, void *context)
{
	void *callstack[128];
	char **strs;
	int frames;

	(void)context;

	frames = backtrace(callstack, 128);
	strs = backtrace_symbols(callstack, frames);

	fprintf(stderr, "\r\nProcess %d got signal %d\r\n"
	    "\tat faultaddr: %p errno: %d code: %d\r\n",
	    siginfo->si_pid, sig, siginfo->si_addr, siginfo->si_errno,
	    siginfo->si_code);
	if (debug_out) {
		fprintf(debug_out, "Process %d got signal %d\n"
		    "\tat faultaddr: %p errno: %d code: %d\n",
		    siginfo->si_pid, sig, siginfo->si_addr, siginfo->si_errno,
		    siginfo->si_code);
	}

	fprintf(stderr, "Backtrace:\r\n");
	if (debug_out) {
		fprintf(debug_out, "Backtrace:\n");
	}

	for (int i = 0; i < frames; ++i) {
		fprintf(stderr, "\n\r%s", strs[i]);
		if (debug_out) {
			fprintf(debug_out, "%s\n", strs[i]);
		}
	}

	fprintf(stderr, "\r\n");
	free(strs);

	if (ioctl(STDOUT_FILENO, TIOCSWINSZ, &rwin) == -1) {
		perror("ioctl(TIOCSWINSZ)");
	}

	if (tcsetattr(STDOUT_FILENO, TCSAFLUSH, &tt) == -1) {
		perror("tcsetattr");
	}

	exit(EXIT_FAILURE);
}

static void
handle_sigchld(int sig)
{
	int status;
	pid_t pid;

	(void)sig;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0);
}

static void
handle_sigwinch(int sig)
{
	unsigned short minrow, mincol;

	(void)sig;

	/* Allow resizes, but only if they are smaller than our original window
	 * size. In any case, restore to the current window size.
	 */
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &rwin) == -1) {
		perror("ioctl(TIOCGWINSZ)");
		exit(EXIT_FAILURE);
	}

	minrow = MIN(rwin.ws_row, owin.ws_row);
	mincol = MIN(rwin.ws_col, owin.ws_col);

	if (win.ws_row != minrow || win.ws_col != mincol) {
		win.ws_row = minrow;
		win.ws_col = mincol;

		if (ioctl(masterfd, TIOCSWINSZ, &win) == -1) {
			perror("ioctl(TIOCSWINSZ)");
			exit(EXIT_FAILURE);
		}
	}
}

void
setup_sighandlers(void)
{
	stack_t ss;

	memset(&ss, 0, sizeof(ss));
	ss.ss_size = 4 * SIGSTKSZ;
	ss.ss_sp = calloc(1, ss.ss_size);
	if (ss.ss_sp == NULL) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	sigaltstack(&ss, 0);

	struct sigaction chld;
	chld.sa_flags = 0;
	(void)sigemptyset(&chld.sa_mask);
	chld.sa_handler = handle_sigchld;
	xsigaction(SIGCHLD, &chld, NULL);

	struct sigaction crash;
	crash.sa_flags = 0;
	(void)sigemptyset(&crash.sa_mask);
	crash.sa_sigaction = do_backtrace;
	xsigaction(SIGSEGV, &crash, NULL);
	xsigaction(SIGBUS, &crash, NULL);
	xsigaction(SIGFPE, &crash, NULL);
	xsigaction(SIGILL, &crash, NULL);
	xsigaction(SIGTRAP, &crash, NULL);
	xsigaction(SIGABRT, &crash, NULL);
	xsigaction(SIGPIPE, &crash, NULL);
	xsigaction(SIGSYS, &crash, NULL);

	struct sigaction winch;
	winch.sa_flags = SA_RESTART;
	(void)sigemptyset(&winch.sa_mask);
	winch.sa_handler = handle_sigwinch;
	xsigaction(SIGWINCH, &winch, NULL);
}


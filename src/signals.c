#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/signal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "castty.h"
#include "record.h"

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

	restore_tty();

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

	exit(EXIT_FAILURE);
}

static void
handle_term(int sig)
{
	if (child > 0) {
		(void)kill(child, sig);
	}
	restore_tty();
	signal(sig, SIG_DFL);
	raise(sig);
}

static void
handle_sigchld(int sig)
{
	int saved_errno = errno;
	int status;
	pid_t pid;

	(void)sig;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (sigchld_write_fd >= 0) {
			ssize_t wr = write(sigchld_write_fd, "", 1);
			(void)wr;
		}
	}

	errno = saved_errno;
}

static void
handle_sigwinch(int sig)
{
	int saved_errno = errno;

	(void)sig;

	/* Allow resizes, but only if they are smaller than our original window
	 * size. In any case, restore to the current window size.
	 */
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &rwin) == -1) {
		errno = saved_errno;
		return;
	}

	record_sync_winsize(ctrl_write_fd, 0, CMD_RESIZE);
	errno = saved_errno;
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
	chld.sa_flags = SA_NOCLDSTOP;
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

	struct sigaction term;
	term.sa_flags = SA_RESETHAND;
	(void)sigemptyset(&term.sa_mask);
	term.sa_handler = handle_term;
	xsigaction(SIGTERM, &term, NULL);
	xsigaction(SIGHUP, &term, NULL);
}


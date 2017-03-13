#define _XOPEN_SOURCE 500
#include <sys/ioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "castty.h"

void
shellproc(const char *shell, const char *exec_cmd, struct winsize *win, int masterfd)
{
	char *pt_name;
	int slavefd;

	slavefd = -1;

	if (setsid() == -1) {
		perror("setsid");
		goto end;
	}

	if (grantpt(masterfd) == -1) {
		perror("grantpt");
		goto end;
	}

	if (unlockpt(masterfd) == -1) {
		perror("unlockpt");
		goto end;
	}

	pt_name = ptsname(masterfd);
	if (pt_name == NULL) {
		perror("ptsname");
		goto end;
	}

	if ((slavefd = open(pt_name, O_RDWR)) < 0) {
		perror("open(ptsname)");
		goto end;
	}

	if (ioctl(slavefd, TIOCSCTTY, 0) == -1) {
		perror("ioctl(TIOCSCTTY)");
		goto end;
	}

	if (ioctl(slavefd, TIOCSWINSZ, win)) {
		perror("ioctl(TIOCSWINSZ)");
		goto end;
	}

	xclose(masterfd);

	xdup2(slavefd, 0);
	xdup2(slavefd, 1);
	xdup2(slavefd, 2);

	xclose(slavefd);

	if (!exec_cmd) {
		execl(shell, strrchr(shell, '/') + 1, "-i", NULL);
	} else {
		execl(shell, strrchr(shell, '/') + 1, "-c", exec_cmd, NULL);
	}

	perror("execl");

end:
	if (kill(0, SIGTERM) == -1) {
		perror("kill");
	}
}

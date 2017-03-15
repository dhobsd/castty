#include <sys/ioctl.h>

#include <stdio.h>
#include <stdlib.h>

#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "castty.h"

void
xclose(int fd)
{

	if (close(fd) == -1) {
		perror("close");
		exit(EXIT_FAILURE);
	}
}

int
xdup2(int oldfd, int newfd)
{
	int fd;
	
	if ((fd = dup2(oldfd, newfd)) == -1) {
		perror("dup2");
		exit(EXIT_FAILURE);
	}
	return fd;
}

void
xfclose(FILE *f)
{

	if (fclose(f) == EOF) {
		perror("fclose");
		exit(EXIT_FAILURE);
	}
}

FILE *
xfopen(const char *f, const char *m)
{
	FILE *r;

	r = fopen(f, m);
	if (r == NULL) {
		perror("fopen");
		exit(EXIT_FAILURE);
	}

	return r;
}

void
xsigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{

	if (sigaction(signum, act, oldact) == -1) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}
}

void
xtcsetattr(int fd, int opt, const struct termios *tio)
{

	if (tcsetattr(fd, opt, tio) == -1) {
		perror("tcsetattr");
		exit(EXIT_FAILURE);
	}
}

size_t
xwrite(int fd, void *buf, size_t size)
{
	ssize_t s;

	if ((s = write(fd, buf, size)) == -1) {
		perror("write");
		exit(EXIT_FAILURE);
	}

	return s;
}

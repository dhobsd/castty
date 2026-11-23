#include <sys/ioctl.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "castty.h"

char *
json_escape(const char *from)
{
	unsigned char c;
	size_t i, o, len;
	char *p;

	assert(from);

	len = strlen(from);
	p = malloc(len * 6 + 1);
	if (p == NULL) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}

	for (i = 0, o = 0; i < len; i++) {
		c = (unsigned char)from[i];
		switch (c) {
		case '\\':
		case '"':
			p[o++] = '\\';
			p[o++] = (char)c;
			break;
		case '\b':
			p[o++] = '\\';
			p[o++] = 'b';
			break;
		case '\f':
			p[o++] = '\\';
			p[o++] = 'f';
			break;
		case '\n':
			p[o++] = '\\';
			p[o++] = 'n';
			break;
		case '\r':
			p[o++] = '\\';
			p[o++] = 'r';
			break;
		case '\t':
			p[o++] = '\\';
			p[o++] = 't';
			break;
		default:
			if (c < 0x20) {
				o += sprintf(&p[o], "\\u%04x", c);
			} else {
				p[o++] = (char)c;
			}
			break;
		}
	}

	p[o] = '\0';

	return p;
}

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
xtcgetattr(int fd, struct termios *tio)
{

	if (tcgetattr(fd, tio) == -1) {
		perror("tcgetattr");
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

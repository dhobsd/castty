/* See LICENSE for redistribution information */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "endian.h"
#include "ttyrec.h"

int
read_header(FILE *fp, Header *h)
{
	uint32_t buf[3];

	if (fread(buf, sizeof buf, 1, fp) == 0) {
		return 0;
	}

	h->tv.tv_sec	= le32toh(buf[0]);
	h->tv.tv_usec	= le32toh(buf[1]);
	h->len		= le32toh(buf[2]);

	return 1;
}

int
write_header(FILE *fp, Header *h)
{
	static struct timeval first;
	static double fms;

	struct timeval now;
	double nms;

	if (first.tv_sec == 0) {
		first = h->tv;
		fms = (double)(first.tv_sec * 1000) + ((double)first.tv_usec / 1000.);
		now = first;
		fputs("var _tre=[{", fp);
	} else {
		now = h->tv;
		fputs("\"},{", fp);
	}

	nms = (double)(now.tv_sec * 1000) + ((double)now.tv_usec / 1000.);

	fprintf(fp, "\"s\":%0.3f,\"e\":\"", nms - fms);

	return 1;
}

static char *progname = "";
void
set_progname(const char *name)
{
	progname = strdup(name);
}

FILE *
efopen(const char *path, const char *mode)
{
	FILE *fp = fopen(path, mode);
	if (fp == NULL) {
		perror("fopen");
		exit(EXIT_FAILURE);
	}
	return fp;
}

int
edup(int oldfd)
{
	int fd = dup(oldfd);
	if (fd == -1) {
		perror("dup");
		exit(EXIT_FAILURE);
	}
	return fd;
}

int
edup2(int oldfd, int newfd)
{
	int fd = dup2(oldfd, newfd);
	if (fd == -1) {
		perror("dup2");
		exit(EXIT_FAILURE);
	}
	return fd;
}

FILE *
efdopen(int fd, const char *mode)
{
	FILE *fp = fdopen(fd, mode);
	if (fp == NULL) {
		perror("fdopen");
		exit(EXIT_FAILURE);
	}
	return fp;
}

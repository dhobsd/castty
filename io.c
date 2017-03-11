/* See LICENSE for redistribution information */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "castty.h"

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

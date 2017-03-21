#ifndef CASTTY_H
#define CASTTY_H

#include <stdio.h>

#include <signal.h>
#include <termios.h>

void setup_sighandlers(void);

void xclose(int);
int xdup2(int, int);
void xfclose(FILE *);
FILE *xfopen(const char *, const char *);
void xsigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
void xtcsetattr(int, int, const struct termios *);
size_t xwrite(int, void *, size_t);

#ifndef MIN
#define MIN(a, b) ((a < b) ? a : b)
#endif

#endif

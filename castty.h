#ifndef CASTTY_H
#define CASTTY_H

#include <sys/ioctl.h>

#include <stddef.h>
#include <stdio.h>

#include <termios.h>

void audio_start(const char *, int);
void audio_toggle(void);
void audio_deinit(void);

void inputproc(int, int);
void outputproc(int, int, const char *, const char *, int, int, int);
void shellproc(const char *, const char *, struct winsize *, struct termios *, int);

void xclose(int);
int xdup2(int, int);
void xfclose(FILE *);
void xtcsetattr(int, int, const struct termios *);
size_t xwrite(int, void *, size_t);

#endif

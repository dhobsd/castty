#ifndef CASTTY_H
#define CASTTY_H

#include <sys/ioctl.h>

#include <stddef.h>
#include <stdio.h>

#include <termios.h>

void inputproc(int, int);
void outputproc(int, int, const char *, const char *, const char *, int, int, int);
void shellproc(const char *, const char *, struct winsize *, int);

double audio_clock_ms(void);
void audio_list(void);
void audio_mute(void);
void audio_start(const char *devid, const char *outfile, int append);
void audio_stop(void);
void audio_toggle_mute(void);
void audio_toggle_pause(void);

void xclose(int);
int xdup2(int, int);
void xfclose(FILE *);
FILE *xfopen(const char *, const char *);
void xtcsetattr(int, int, const struct termios *);
size_t xwrite(int, void *, size_t);

#endif

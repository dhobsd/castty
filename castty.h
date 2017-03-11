#ifndef CASTTY_H
#define CASTTY_H

FILE*   efopen          (const char *path, const char *mode);
int     edup            (int oldfd);
int     edup2           (int oldfd, int newfd);
FILE*   efdopen         (int fd, const char *mode);

void audio_init(const char *, int);
void audio_toggle(void);
void audio_deinit(void);

#endif

#ifndef CASTTY_H
#define CASTTY_H

#include <sys/time.h>

typedef struct header {
    struct timeval tv;
    int len;
} Header;

int     read_header     (FILE *fp, Header *h);
int     write_header    (FILE *fp, Header *h);
FILE*   efopen          (const char *path, const char *mode);
int     edup            (int oldfd);
int     edup2           (int oldfd, int newfd);
FILE*   efdopen         (int fd, const char *mode);

void audio_init(const char *, int);
void audio_toggle(void);
void audio_deinit(void);

#endif

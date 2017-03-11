#ifndef CASTTY_H
#define CASTTY_H

int edup(int oldfd);
int edup2(int oldfd, int newfd);
FILE *efopen(const char *path, const char *mode);
FILE *efdopen(int fd, const char *mode);

void audio_init(const char *outfile, int flag);
void audio_toggle(void);
void audio_deinit(void);

#endif

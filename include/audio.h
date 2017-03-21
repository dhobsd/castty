#ifndef AUDIO_H
#define AUDIO_H

double audio_clock_ms(void);
void audio_exit(void);
void audio_list(void);
void audio_mute(void);
void audio_init(const char *devid, const char *outfile);
void audio_start(void);
void audio_stop(void);
void audio_toggle_mp3(void);
void audio_toggle_mute(void);
void audio_toggle_pause(void);


#endif

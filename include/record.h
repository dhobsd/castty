#ifndef RECORD_H
#define RECORD_H

#include <sys/ioctl.h>

enum control_command {
	CMD_NONE,
	CMD_CTRL_A,
	CMD_MUTE,
	CMD_PAUSE,
};

struct outargs {
	int start_paused;
	int controlfd;
	int masterfd;
	int rows;
	int cols;
	int format_version;
	int use_raw;

	const char *cmd;
	const char *env;
	const char *title;
	const char *outfn;
	const char *devid;
	const char *audioout;
};

int record_main(int, char **);

void inputproc(int, int);
void outputproc(struct outargs *oa);
void shellproc(const char *, const char *, struct winsize *, int);

#endif

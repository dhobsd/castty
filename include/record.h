#ifndef RECORD_H
#define RECORD_H

#include <sys/ioctl.h>
#include <sys/types.h>

#include <signal.h>
#include <time.h>

enum control_command {
	CMD_NONE,
	CMD_CTRL_A,
	CMD_MUTE,
	CMD_PAUSE,
	CMD_RESIZE,
};

struct control_msg {
	enum control_command cmd;
	unsigned short rows;
	unsigned short cols;
};

extern volatile sig_atomic_t ctrl_write_fd;

struct outargs {
	int start_paused;
	int controlfd;
	int masterfd;
	int rows;
	int cols;
	int format_version;
	int use_raw;
	pid_t shellpid;
	time_t timestamp;

	const char *cmd;
	const char *env;
	const char *term_type;
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

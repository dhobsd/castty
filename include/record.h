#ifndef RECORD_H
#define RECORD_H

#include <sys/ioctl.h>
#include <sys/types.h>

#include <signal.h>
#include <time.h>

#include "cast.h"

enum control_command {
	CMD_NONE,
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
extern volatile sig_atomic_t sigchld_write_fd;
extern volatile sig_atomic_t tty_saved;

struct outargs {
	int start_paused;
	int append;
	int controlfd;
	int masterfd;
	int rows;
	int cols;
	int format_version;
	int use_raw;
	pid_t shellpid;
	time_t timestamp;
	struct cast_append_info append_info;

	const char *cmd;
	const char *env;
	const char *term_type;
	const char *term_version;
	const char *theme_json;
	const char *title;
	const char *outfn;
	const char *devid;
	const char *audioout;
};

int record_main(int, char **);
void restore_tty(void);

void inputproc(int, int, int);
void outputproc(struct outargs *oa);
void shellproc(const char *, const char *, struct winsize *, int);

#endif

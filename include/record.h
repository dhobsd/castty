#ifndef RECORD_H
#define RECORD_H

#include <sys/ioctl.h>
#include <sys/types.h>

#include <signal.h>
#include <time.h>

#include "cast.h"
#include "tui/hud.h"

enum control_command {
	CMD_NONE,
	CMD_MUTE,
	CMD_PAUSE,
	CMD_RESIZE,
	CMD_MARKER,
	CMD_HUD_TOGGLE,
};

struct control_msg {
	enum control_command cmd;
	unsigned short rows;
	unsigned short cols;
	unsigned short outer_rows;
	unsigned short outer_cols;
	enum hud_mode hud_mode;
};

extern volatile sig_atomic_t ctrl_write_fd;
extern volatile sig_atomic_t sigchld_write_fd;
extern volatile sig_atomic_t tty_saved;
extern enum hud_mode rec_hud_mode;
extern int rec_req_rows, rec_req_cols;
extern volatile sig_atomic_t hud_user_hidden;

void record_sync_winsize(int ctrl_fd, int always_send,
    enum control_command cmd);
void record_init_winsize_cache(int outer_rows, int outer_cols,
    enum hud_mode mode);

struct outargs {
	int start_paused;
	int append;
	int controlfd;
	int masterfd;
	int rows;
	int cols;
	int outer_rows;
	int outer_cols;
	int hud_enabled;
	enum hud_mode hud_mode;
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

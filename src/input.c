#include <sys/ioctl.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "castty.h"
#include "record.h"

extern struct winsize rwin;

static ssize_t
read_input(int chldfd, unsigned char *buf, size_t len)
{
	struct pollfd pfds[2] = {
		{ .fd = STDIN_FILENO, .events = POLLIN },
		{ .fd = chldfd, .events = POLLIN },
	};

	for (;;) {
		int ready = poll(pfds, chldfd >= 0 ? 2 : 1, -1);

		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		if (chldfd >= 0 &&
		    (pfds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
			return 0;
		}
		if (pfds[0].revents & POLLNVAL) {
			return -1;
		}
		if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			ssize_t nread = read(STDIN_FILENO, buf, len);

			if (nread < 0 && errno == EINTR) {
				continue;
			}
			return nread;
		}
	}
}

void
inputproc(int masterfd, int controlfd, int chldfd)
{
	unsigned char ibuf[BUFSIZ], *p;
	enum input_state {
		STATE_PASSTHROUGH,
		STATE_COMMAND,
	} input_state;
	ssize_t nread;

	input_state = STATE_PASSTHROUGH;

	while ((nread = read_input(chldfd, ibuf, BUFSIZ)) > 0) {
		p = ibuf;

		while (nread > 0) {
			switch (input_state) {
			case STATE_PASSTHROUGH: {
				unsigned char *cmdstart = memchr(p, 0x01,
				    (size_t)nread);

				if (cmdstart == NULL) {
					xwrite(masterfd, p, (size_t)nread);
					nread = 0;
				} else {
					ssize_t prefix = cmdstart - p;

					if (prefix > 0) {
						xwrite(masterfd, p,
						    (size_t)prefix);
					}
					p = cmdstart + 1;
					nread -= prefix + 1;
					input_state = STATE_COMMAND;
				}
				break;
			}

			case STATE_COMMAND: {
				enum control_command cmd = CMD_NONE;

				switch (*p) {
				/* Passthrough a literal ^a in stream order */
				case 'a':
				case 0x01: {
					static unsigned char c_a = 0x01;

					xwrite(masterfd, &c_a, 1);
					break;
				}
				case 'p':
					cmd = CMD_PAUSE;
					break;
				case 'm':
					cmd = CMD_MUTE;
					break;
				case 'k':
					cmd = CMD_MARKER;
					break;
				case 'h':
					if (rec_hud_mode != HUD_OFF) {
						sigset_t s, old;

						sigemptyset(&s);
						sigaddset(&s, SIGWINCH);
						sigprocmask(SIG_BLOCK, &s, &old);
						hud_user_hidden = !hud_user_hidden;
						(void)ioctl(STDIN_FILENO,
						    TIOCGWINSZ, &rwin);
						record_sync_winsize(controlfd,
						    1, CMD_HUD_TOGGLE);
						sigprocmask(SIG_SETMASK, &old,
						    NULL);
					}
					break;
				default:
					break;
				}

				if (cmd != CMD_NONE) {
					struct control_msg msg = {
						.cmd = cmd,
						.rows = 0,
						.cols = 0,
					};
					xwrite(controlfd, &msg, sizeof msg);
				}

				p++;
				nread--;
				input_state = STATE_PASSTHROUGH;
				break;
			}
			}
		}
	}
}

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

static void
send_marker_msg(int controlfd, enum control_command cmd, int active,
    const char *label)
{
	struct control_msg msg;

	memset(&msg, 0, sizeof msg);
	msg.cmd = cmd;
	msg.prompt_active = (unsigned char)(active ? 1 : 0);
	if (label != NULL) {
		snprintf(msg.label, sizeof msg.label, "%s", label);
	}
	xwrite(controlfd, &msg, sizeof msg);
}

static int
stdin_has_byte_within_ms(int timeout_ms)
{
	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };

	for (;;) {
		int rc = poll(&pfd, 1, timeout_ms);

		if (rc < 0 && errno == EINTR) {
			continue;
		}
		return rc > 0 && (pfd.revents & POLLIN);
	}
}

void
inputproc(int masterfd, int controlfd, int chldfd)
{
	unsigned char ibuf[BUFSIZ], *p;
	char marker_buf[64];
	size_t marker_len = 0;
	enum input_state {
		STATE_PASSTHROUGH,
		STATE_COMMAND,
		STATE_MARKER_PROMPT,
		STATE_MARKER_ESC,
		STATE_MARKER_CSI,
		STATE_MARKER_SS3,
	} input_state;
	ssize_t nread;

	marker_buf[0] = '\0';
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
				enum input_state next_state = STATE_PASSTHROUGH;

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
				case 'K':
					marker_len = 0;
					marker_buf[0] = '\0';
					send_marker_msg(controlfd,
					    CMD_MARKER_PROMPT, 1, marker_buf);
					next_state = STATE_MARKER_PROMPT;
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
					struct control_msg msg;

					memset(&msg, 0, sizeof msg);
					msg.cmd = cmd;
					xwrite(controlfd, &msg, sizeof msg);
				}

				p++;
				nread--;
				input_state = next_state;
				break;
			}

			case STATE_MARKER_PROMPT: {
				unsigned char ch = *p++;
				nread--;

				if (ch == '\r' || ch == '\n') {
					send_marker_msg(controlfd, CMD_MARKER,
					    0, marker_buf);
					marker_len = 0;
					marker_buf[0] = '\0';
					input_state = STATE_PASSTHROUGH;
				} else if (ch == 0x03 || ch == 0x07) {
					marker_len = 0;
					marker_buf[0] = '\0';
					send_marker_msg(controlfd,
					    CMD_MARKER_PROMPT, 0, "");
					input_state = STATE_PASSTHROUGH;
				} else if (ch == 0x1b) {
					if (nread > 0 ||
					    stdin_has_byte_within_ms(25)) {
						input_state = STATE_MARKER_ESC;
					} else {
						marker_len = 0;
						marker_buf[0] = '\0';
						send_marker_msg(controlfd,
						    CMD_MARKER_PROMPT, 0, "");
						input_state = STATE_PASSTHROUGH;
					}
				} else if (ch == 0x7f || ch == 0x08) {
					while (marker_len > 0 &&
					    ((unsigned char)marker_buf[marker_len - 1] &
					    0xc0) == 0x80) {
						marker_len--;
					}
					if (marker_len > 0) {
						marker_len--;
					}
					marker_buf[marker_len] = '\0';
					send_marker_msg(controlfd,
					    CMD_MARKER_PROMPT, 1, marker_buf);
				} else if (ch == 0x15) {
					marker_len = 0;
					marker_buf[0] = '\0';
					send_marker_msg(controlfd,
					    CMD_MARKER_PROMPT, 1, marker_buf);
				} else if (ch >= 0x20 && ch != 0x7f) {
					if (marker_len + 1 < sizeof marker_buf) {
						marker_buf[marker_len++] =
						    (char)ch;
						marker_buf[marker_len] = '\0';
					}
					if (nread == 0 || *p < 0x20 ||
					    *p == 0x7f) {
						send_marker_msg(controlfd,
						    CMD_MARKER_PROMPT, 1,
						    marker_buf);
					}
				}
				break;
			}

			case STATE_MARKER_ESC: {
				unsigned char ch = *p++;
				nread--;

				if (ch == '[') {
					input_state = STATE_MARKER_CSI;
				} else if (ch == 'O') {
					input_state = STATE_MARKER_SS3;
				} else {
					marker_len = 0;
					marker_buf[0] = '\0';
					send_marker_msg(controlfd,
					    CMD_MARKER_PROMPT, 0, "");
					input_state = STATE_PASSTHROUGH;
				}
				break;
			}

			case STATE_MARKER_CSI: {
				unsigned char ch = *p++;
				nread--;

				if (ch >= 0x40 && ch <= 0x7e) {
					input_state = STATE_MARKER_PROMPT;
				}
				break;
			}

			case STATE_MARKER_SS3:
				p++;
				nread--;
				input_state = STATE_MARKER_PROMPT;
				break;
			}
		}
	}
}

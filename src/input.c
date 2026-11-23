#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "castty.h"
#include "record.h"

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
		unsigned char *cmdstart;

		p = ibuf;

		cmdstart = memchr(ibuf, 0x01, nread);
		if (cmdstart) {
			switch (input_state) {
			case STATE_PASSTHROUGH:
				/* Switching into command mode: pass through anything
				 * preceding our command
				 */
				if (cmdstart > ibuf) {
					xwrite(masterfd, ibuf, cmdstart - ibuf);
				}
				cmdstart++;
				nread -= (cmdstart - ibuf);

				p = cmdstart;
				input_state = STATE_COMMAND;

				break;
			case STATE_COMMAND:
				break;
			}
		}

		switch (input_state) {
		case STATE_PASSTHROUGH:
			xwrite(masterfd, ibuf, nread);
			break;

		case STATE_COMMAND:
			if (nread) {
				enum control_command cmd = CMD_NONE;

				switch (*p) {
				/* Passthrough a literal ^a */
				case 'a':
				case  0x01:
					cmd = CMD_CTRL_A;
					break;
				case 'p':
					cmd = CMD_PAUSE;
					break;
				case 'm':
					cmd = CMD_MUTE;
					break;
				default:
					input_state = STATE_PASSTHROUGH;
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

				if (nread > 1) {
					xwrite(masterfd, p + 1, nread - 1);
				}

				input_state = STATE_PASSTHROUGH;
			}
			break;
		}

	}
}

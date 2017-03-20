#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "castty.h"

void
inputproc(int masterfd, int controlfd)
{
	unsigned char ibuf[BUFSIZ], *p;
	enum input_state {
		STATE_PASSTHROUGH,
		STATE_COMMAND,
	} input_state;
	ssize_t nread;

	input_state = STATE_PASSTHROUGH;

	while ((nread = read(STDIN_FILENO, ibuf, BUFSIZ)) > 0) {
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
					xwrite(controlfd, &cmd, sizeof cmd);
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

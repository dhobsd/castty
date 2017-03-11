#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "castty.h"

void
inputproc(int masterfd, int controlfd)
{
	unsigned char ibuf[BUFSIZ], *p;
	unsigned cmdbytes;
	enum input_state {
		STATE_PASSTHROUGH,
		STATE_COMMAND,
	} input_state;
	ssize_t cc;

	input_state = STATE_PASSTHROUGH;
	cmdbytes = 0;

	while ((cc = read(STDIN_FILENO, ibuf, BUFSIZ)) > 0) {
		unsigned char *cmdstart;
		unsigned char *cmdend;

		p = ibuf;

		cmdstart = memchr(ibuf, 0x01, cc);
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
				cc -= (cmdstart - ibuf);

				p = cmdstart;
				input_state = STATE_COMMAND;

				break;
			case STATE_COMMAND:
				break;
			}
		}

		switch (input_state) {
		case STATE_PASSTHROUGH:
			xwrite(masterfd, ibuf, cc);
			break;

		case STATE_COMMAND:
			cmdend = memchr(p, '\r', cc);
			if (cmdend) {
				xwrite(controlfd, p, cmdend - p + 1);
				cmdend++;

				if (cmdend - p + 1 < cc) {
					xwrite(masterfd, p + 1, cc - (cmdend - p));
				}

				input_state = STATE_PASSTHROUGH;
				cmdbytes = 0;
			} else {
				if (cc) {
					/* Hack. ^aa and ^a^a send a literal ^a without
					 * causing the output side to go into command mode.
					 */
					if (cmdbytes == 0 && (*p == 'a' || *p == 0x01)) {
						char byte = 0x01;
						xwrite(masterfd, &byte, 1);

						input_state = STATE_PASSTHROUGH;
					} else {
						cmdbytes += cc;
						xwrite(controlfd, p, cc);
					}
				}
			}

			break;
		}

	}
}

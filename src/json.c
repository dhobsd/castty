#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

char *
json_escape(const char *from)
{
	unsigned char c;
	size_t i, o, len;
	char *p;

	assert(from);

	len = strlen(from);
	p = malloc(len * 6 + 1);
	if (p == NULL) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}

	for (i = 0, o = 0; i < len; i++) {
		c = (unsigned char)from[i];
		switch (c) {
		case '\\':
		case '"':
			p[o++] = '\\';
			p[o++] = (char)c;
			break;
		case '\b':
			p[o++] = '\\';
			p[o++] = 'b';
			break;
		case '\f':
			p[o++] = '\\';
			p[o++] = 'f';
			break;
		case '\n':
			p[o++] = '\\';
			p[o++] = 'n';
			break;
		case '\r':
			p[o++] = '\\';
			p[o++] = 'r';
			break;
		case '\t':
			p[o++] = '\\';
			p[o++] = 't';
			break;
		default:
			if (c < 0x20) {
				o += sprintf(&p[o], "\\u%04x", c);
			} else {
				p[o++] = (char)c;
			}
			break;
		}
	}

	p[o] = '\0';

	return p;
}

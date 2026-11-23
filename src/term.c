#include <sys/time.h>

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "castty.h"
#include "json.h"
#include "term.h"

enum {
	PROBE_TIMEOUT_MS = 150,
	PALETTE_SIZE = 16,
};

enum parse_state {
	PS_GROUND,
	PS_ESC,
	PS_CSI,
	PS_OSC,
	PS_OSC_ESC,
	PS_DCS,
	PS_DCS_ESC,
};

struct probe_ctx {
	char fg[8];
	char bg[8];
	char pal[PALETTE_SIZE][8];
	int has_fg;
	int has_bg;
	int has_pal[PALETTE_SIZE];
	char *raw_version;
	int da1_seen;
};

static int
hex_val(unsigned char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return 10 + (c - 'a');
	}
	if (c >= 'A' && c <= 'F') {
		return 10 + (c - 'A');
	}
	return -1;
}

static int
parse_hex_component(const char **sp, char sep)
{
	const char *p = *sp;
	unsigned int val = 0;
	int digits = 0;

	while (isxdigit((unsigned char)*p) && digits < 4) {
		val = (val << 4) | (unsigned int)hex_val((unsigned char)*p);
		p++;
		digits++;
	}
	if (digits == 0 || *p != sep) {
		return -1;
	}
	*sp = sep ? (p + 1) : p;

	switch (digits) {
	case 1:
		return (int)((val << 4) | val);
	case 2:
		return (int)val;
	case 3:
		return (int)(val >> 4);
	case 4:
		return (int)(val >> 8);
	default:
		return -1;
	}
}

static int
parse_xcolor(const char *s, char out_hex[8])
{
	int r, g, b;

	if (strncmp(s, "rgb:", 4) == 0) {
		const char *p = s + 4;

		r = parse_hex_component(&p, '/');
		g = (r >= 0) ? parse_hex_component(&p, '/') : -1;
		b = (g >= 0) ? parse_hex_component(&p, '\0') : -1;
		if (b < 0) {
			return -1;
		}
		snprintf(out_hex, 8, "#%02x%02x%02x",
		    (unsigned)r & 0xff, (unsigned)g & 0xff, (unsigned)b & 0xff);
		return 0;
	}

	if (s[0] == '#') {
		size_t hlen = strlen(s + 1);
		if (hlen == 6) {
			for (size_t i = 0; i < 6; i++) {
				if (!isxdigit((unsigned char)s[1 + i])) {
					return -1;
				}
				out_hex[1 + i] = (char)tolower((unsigned char)s[1 + i]);
			}
			out_hex[0] = '#';
			out_hex[7] = '\0';
			return 0;
		}
		if (hlen == 12) {
			for (size_t i = 0; i < 12; i++) {
				if (!isxdigit((unsigned char)s[1 + i])) {
					return -1;
				}
			}
			out_hex[0] = '#';
			out_hex[1] = (char)tolower((unsigned char)s[1]);
			out_hex[2] = (char)tolower((unsigned char)s[2]);
			out_hex[3] = (char)tolower((unsigned char)s[5]);
			out_hex[4] = (char)tolower((unsigned char)s[6]);
			out_hex[5] = (char)tolower((unsigned char)s[9]);
			out_hex[6] = (char)tolower((unsigned char)s[10]);
			out_hex[7] = '\0';
			return 0;
		}
	}

	return -1;
}

static void
handle_osc(const char *buf, struct probe_ctx *ctx)
{
	if (strncmp(buf, "10;", 3) == 0) {
		if (parse_xcolor(buf + 3, ctx->fg) == 0) {
			ctx->has_fg = 1;
		}
	} else if (strncmp(buf, "11;", 3) == 0) {
		if (parse_xcolor(buf + 3, ctx->bg) == 0) {
			ctx->has_bg = 1;
		}
	} else if (strncmp(buf, "4;", 2) == 0) {
		char *end = NULL;
		unsigned long idx;

		if (!isdigit((unsigned char)buf[2])) {
			return;
		}
		idx = strtoul(buf + 2, &end, 10);
		if (end && *end == ';' && idx < PALETTE_SIZE) {
			if (parse_xcolor(end + 1, ctx->pal[idx]) == 0) {
				ctx->has_pal[idx] = 1;
			}
		}
	}
}

static void
handle_dcs(const char *buf, struct probe_ctx *ctx)
{
	const char *ver;
	size_t len;

	if (buf[0] != '>' || buf[1] != '|') {
		return;
	}
	ver = buf + 2;
	len = strlen(ver);
	while (len > 0 && (unsigned char)ver[len - 1] <= ' ') {
		len--;
	}
	if (len == 0) {
		return;
	}
	free(ctx->raw_version);
	ctx->raw_version = malloc(len + 1);
	if (ctx->raw_version == NULL) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	memcpy(ctx->raw_version, ver, len);
	ctx->raw_version[len] = '\0';
}

static int
write_all(int fd, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static long
elapsed_ms(const struct timeval *start, const struct timeval *now)
{
	return (now->tv_sec - start->tv_sec) * 1000L +
	    (now->tv_usec - start->tv_usec) / 1000L;
}

static char *
build_theme_json(const struct probe_ctx *ctx)
{
	int count = 0;
	size_t sz, off;
	char *json;

	if (!ctx->has_fg || !ctx->has_bg) {
		return NULL;
	}

	while (count < PALETTE_SIZE && ctx->has_pal[count]) {
		count++;
	}
	if (count == PALETTE_SIZE) {
		count = 16;
	} else if (count >= 8) {
		count = 8;
	} else {
		return NULL;
	}

	/* {"fg":"#rrggbb","bg":"#rrggbb","palette":"#rrggbb:..."} */
	sz = 64 + (size_t)count * 8;
	json = malloc(sz);
	if (json == NULL) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}

	off = (size_t)snprintf(json, sz,
	    "{\"fg\":\"%s\",\"bg\":\"%s\",\"palette\":\"",
	    ctx->fg, ctx->bg);
	for (int i = 0; i < count; i++) {
		off += (size_t)snprintf(json + off, sz - off, "%s%s",
		    i > 0 ? ":" : "", ctx->pal[i]);
	}
	snprintf(json + off, sz - off, "\"}");
	return json;
}

void
term_probe_tty(int infd, int outfd, const char *term_type,
    struct term_probe *out)
{
	char qbuf[256];
	size_t qlen = 0;
	struct probe_ctx ctx;
	enum parse_state state = PS_GROUND;
	char seqbuf[256];
	size_t seqlen = 0;
	struct timeval tstart, tnow;

	memset(out, 0, sizeof *out);
	if (!isatty(infd) || !isatty(outfd) || !term_type || !*term_type ||
	    strcmp(term_type, "dumb") == 0) {
		return;
	}

	memset(&ctx, 0, sizeof ctx);

	qlen += (size_t)snprintf(qbuf + qlen, sizeof qbuf - qlen,
	    "\x1b[>0q\x1b]10;?\x07\x1b]11;?\x07");
	for (int i = 0; i < PALETTE_SIZE; i++) {
		qlen += (size_t)snprintf(qbuf + qlen, sizeof qbuf - qlen,
		    "\x1b]4;%d;?\x07", i);
	}
	qlen += (size_t)snprintf(qbuf + qlen, sizeof qbuf - qlen, "\x1b[c");

	if (write_all(outfd, qbuf, qlen) == -1) {
		return;
	}

	gettimeofday(&tstart, NULL);

	while (!ctx.da1_seen) {
		struct pollfd pfd;
		long rem;
		int pr;

		gettimeofday(&tnow, NULL);
		rem = PROBE_TIMEOUT_MS - elapsed_ms(&tstart, &tnow);
		if (rem <= 0) {
			break;
		}

		pfd.fd = infd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		pr = poll(&pfd, 1, (int)rem);
		if (pr < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (pr == 0 || !(pfd.revents & POLLIN)) {
			break;
		}

		unsigned char rbuf[512];
		ssize_t nr = read(infd, rbuf, sizeof rbuf);
		if (nr <= 0) {
			break;
		}

		for (ssize_t i = 0; i < nr; i++) {
			unsigned char ch = rbuf[i];

			switch (state) {
			case PS_GROUND:
				if (ch == 0x1b) {
					state = PS_ESC;
				} else if (ch == 0x9b) {
					seqlen = 0;
					state = PS_CSI;
				} else if (ch == 0x9d) {
					seqlen = 0;
					state = PS_OSC;
				} else if (ch == 0x90) {
					seqlen = 0;
					state = PS_DCS;
				}
				break;

			case PS_ESC:
				if (ch == '[') {
					seqlen = 0;
					state = PS_CSI;
				} else if (ch == ']') {
					seqlen = 0;
					state = PS_OSC;
				} else if (ch == 'P') {
					seqlen = 0;
					state = PS_DCS;
				} else if (ch != 0x1b) {
					state = PS_GROUND;
				}
				break;

			case PS_CSI:
				if (ch == 0x1b) {
					state = PS_ESC;
				} else if (ch >= 0x40 && ch <= 0x7e) {
					if (ch == 'c' && (seqlen == 0 ||
					    (seqbuf[0] != '>' && seqbuf[0] != '='))) {
						ctx.da1_seen = 1;
					}
					state = PS_GROUND;
				} else if (seqlen + 1 < sizeof seqbuf) {
					seqbuf[seqlen++] = (char)ch;
				}
				break;

			case PS_OSC:
				if (ch == 0x07) {
					seqbuf[seqlen] = '\0';
					handle_osc(seqbuf, &ctx);
					state = PS_GROUND;
				} else if (ch == 0x1b) {
					state = PS_OSC_ESC;
				} else if (seqlen + 1 < sizeof seqbuf) {
					seqbuf[seqlen++] = (char)ch;
				}
				break;

			case PS_OSC_ESC:
				seqbuf[seqlen] = '\0';
				handle_osc(seqbuf, &ctx);
				if (ch == '\\') {
					state = PS_GROUND;
				} else if (ch == 0x1b) {
					state = PS_ESC;
				} else if (ch == '[') {
					seqlen = 0;
					state = PS_CSI;
				} else if (ch == ']') {
					seqlen = 0;
					state = PS_OSC;
				} else if (ch == 'P') {
					seqlen = 0;
					state = PS_DCS;
				} else {
					state = PS_GROUND;
				}
				break;

			case PS_DCS:
				if (ch == 0x07) {
					seqbuf[seqlen] = '\0';
					handle_dcs(seqbuf, &ctx);
					state = PS_GROUND;
				} else if (ch == 0x1b) {
					state = PS_DCS_ESC;
				} else if (seqlen + 1 < sizeof seqbuf) {
					seqbuf[seqlen++] = (char)ch;
				}
				break;

			case PS_DCS_ESC:
				seqbuf[seqlen] = '\0';
				handle_dcs(seqbuf, &ctx);
				if (ch == '\\') {
					state = PS_GROUND;
				} else if (ch == 0x1b) {
					state = PS_ESC;
				} else if (ch == '[') {
					seqlen = 0;
					state = PS_CSI;
				} else if (ch == ']') {
					seqlen = 0;
					state = PS_OSC;
				} else if (ch == 'P') {
					seqlen = 0;
					state = PS_DCS;
				} else {
					state = PS_GROUND;
				}
				break;
			}
		}
	}

	if (ctx.raw_version != NULL) {
		out->version = json_escape(ctx.raw_version);
		free(ctx.raw_version);
	}
	out->theme_json = build_theme_json(&ctx);
}

void
term_probe_free(struct term_probe *probe)
{
	if (!probe) {
		return;
	}
	free(probe->version);
	free(probe->theme_json);
	probe->version = NULL;
	probe->theme_json = NULL;
}

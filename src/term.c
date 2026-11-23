#include <sys/time.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json.h"
#include "term.h"

static char reset_buf[2048];
static size_t reset_len;
static volatile sig_atomic_t reset_ready;

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

enum {
	TI_MAGIC_16 = 0x011a,
	TI_MAGIC_32 = 0x021e,
	TI_MAX_FILE = 32768,
	TI_CURSOR_NORMAL = 16,
	TI_EXIT_ALT_CHARSET = 38,
	TI_EXIT_ATTR = 39,
	TI_EXIT_CA_MODE = 40,
	TI_INIT_1 = 48,
	TI_INIT_2 = 49,
	TI_INIT_3 = 50,
	TI_KEYPAD_LOCAL = 88,
	TI_RESET_1 = 122,
	TI_RESET_2 = 123,
	TI_RESET_3 = 124,
};

struct ti_table {
	const unsigned char *buf;
	size_t str_pos;
	int str_cnt;
	size_t tbl_pos;
	size_t tbl_sz;
};

static uint16_t
ti_u16le(const unsigned char *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t
ti_s16le(const unsigned char *p)
{
	return (int16_t)ti_u16le(p);
}

static int
ti_parse_header(const unsigned char *tbuf, size_t tlen, struct ti_table *tbl)
{
	uint16_t magic;
	int16_t name_sz, bool_sz, num_cnt, str_cnt, tbl_sz;
	size_t num_bytes, pos, tbl_pos;

	if (tlen < 12) {
		return -1;
	}

	magic = ti_u16le(tbuf);
	if (magic == TI_MAGIC_16) {
		num_bytes = 2;
	} else if (magic == TI_MAGIC_32) {
		num_bytes = 4;
	} else {
		return -1;
	}

	name_sz = ti_s16le(tbuf + 2);
	bool_sz = ti_s16le(tbuf + 4);
	num_cnt = ti_s16le(tbuf + 6);
	str_cnt = ti_s16le(tbuf + 8);
	tbl_sz = ti_s16le(tbuf + 10);
	if (name_sz < 0 || bool_sz < 0 || num_cnt < 0 ||
	    str_cnt < 0 || tbl_sz < 0) {
		return -1;
	}

	pos = 12 + (size_t)name_sz + (size_t)bool_sz;
	if (pos & 1) {
		pos++;
	}
	if ((size_t)num_cnt > (tlen - pos) / num_bytes) {
		return -1;
	}
	pos += (size_t)num_cnt * num_bytes;

	if ((size_t)str_cnt > (tlen - pos) / 2) {
		return -1;
	}
	tbl_pos = pos + (size_t)str_cnt * 2;
	if ((size_t)tbl_sz > tlen - tbl_pos) {
		return -1;
	}

	tbl->buf = tbuf;
	tbl->str_pos = pos;
	tbl->str_cnt = str_cnt;
	tbl->tbl_pos = tbl_pos;
	tbl->tbl_sz = (size_t)tbl_sz;
	return 0;
}

static const char *
ti_get_str(const struct ti_table *tbl, int idx)
{
	int16_t off;
	const char *str;

	if (idx < 0 || idx >= tbl->str_cnt) {
		return NULL;
	}

	off = ti_s16le(tbl->buf + tbl->str_pos + (size_t)idx * 2);
	if (off < 0 || (size_t)off >= tbl->tbl_sz) {
		return NULL;
	}

	str = (const char *)(tbl->buf + tbl->tbl_pos + (size_t)off);
	if (memchr(str, '\0', tbl->tbl_sz - (size_t)off) == NULL) {
		return NULL;
	}
	return str;
}

static int
try_read_ti_path(const char *path, unsigned char *tbuf, size_t cap,
    size_t *out_len)
{
	int fd;
	size_t off = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -1;
	}

	while (off < cap) {
		ssize_t nr = read(fd, tbuf + off, cap - off);
		if (nr < 0) {
			if (errno == EINTR) {
				continue;
			}
			close(fd);
			return -1;
		}
		if (nr == 0) {
			break;
		}
		off += (size_t)nr;
	}
	close(fd);

	*out_len = off;
	return off >= 12 ? 0 : -1;
}

static int
try_read_ti_dir(const char *dir, const char *term_type, unsigned char *tbuf,
    size_t cap, size_t *out_len, struct ti_table *tbl)
{
	char path[1024];
	unsigned char first = (unsigned char)term_type[0];
	int rv;

	if (!dir || !*dir) {
		return -1;
	}

	/* Linux/BSD style: <dir>/<c>/<TERM> */
	rv = snprintf(path, sizeof path, "%s/%c/%s", dir, first, term_type);
	if (rv > 0 && (size_t)rv < sizeof path &&
	    try_read_ti_path(path, tbuf, cap, out_len) == 0 &&
	    ti_parse_header(tbuf, *out_len, tbl) == 0) {
		return 0;
	}

	/* macOS/Darwin style: <dir>/<xx>/<TERM> */
	rv = snprintf(path, sizeof path, "%s/%02x/%s", dir, first, term_type);
	if (rv > 0 && (size_t)rv < sizeof path &&
	    try_read_ti_path(path, tbuf, cap, out_len) == 0 &&
	    ti_parse_header(tbuf, *out_len, tbl) == 0) {
		return 0;
	}

	return -1;
}

static unsigned char *
load_terminfo(const char *term_type, struct ti_table *tbl)
{
	static const char *sys_dirs[] = {
		"/etc/terminfo",
		"/lib/terminfo",
		"/usr/share/terminfo",
		"/usr/local/share/terminfo",
	};
	unsigned char *tbuf;
	size_t tlen = 0;
	const char *env_val;

	if (!term_type || !*term_type || strchr(term_type, '/') != NULL) {
		return NULL;
	}

	tbuf = malloc(TI_MAX_FILE);
	if (tbuf == NULL) {
		return NULL;
	}

	env_val = getenv("TERMINFO");
	if (env_val && try_read_ti_dir(env_val, term_type, tbuf, TI_MAX_FILE,
	    &tlen, tbl) == 0) {
		return tbuf;
	}

	env_val = getenv("HOME");
	if (env_val && *env_val) {
		char home_ti[1024];
		int rv = snprintf(home_ti, sizeof home_ti, "%s/.terminfo", env_val);
		if (rv > 0 && (size_t)rv < sizeof home_ti &&
		    try_read_ti_dir(home_ti, term_type, tbuf, TI_MAX_FILE,
		    &tlen, tbl) == 0) {
			return tbuf;
		}
	}

	env_val = getenv("TERMINFO_DIRS");
	if (env_val) {
		const char *p = env_val;
		while (*p) {
			const char *colon = strchr(p, ':');
			size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
			char dir[1024];

			if (dlen == 0) {
				snprintf(dir, sizeof dir, "/usr/share/terminfo");
			} else if (dlen < sizeof dir) {
				memcpy(dir, p, dlen);
				dir[dlen] = '\0';
			} else {
				dir[0] = '\0';
			}

			if (dir[0] && try_read_ti_dir(dir, term_type, tbuf,
			    TI_MAX_FILE, &tlen, tbl) == 0) {
				return tbuf;
			}
			if (!colon) {
				break;
			}
			p = colon + 1;
		}
	}

	for (size_t i = 0; i < sizeof sys_dirs / sizeof sys_dirs[0]; i++) {
		if (try_read_ti_dir(sys_dirs[i], term_type, tbuf, TI_MAX_FILE,
		    &tlen, tbl) == 0) {
			return tbuf;
		}
	}

	free(tbuf);
	return NULL;
}

static void
append_raw_bytes(const char *s, size_t len)
{
	size_t avail = sizeof reset_buf - reset_len;
	size_t n = len < avail ? len : avail;

	if (n > 0) {
		memcpy(reset_buf + reset_len, s, n);
		reset_len += n;
	}
}

static void
append_ti_cap(const struct ti_table *tbl, int idx, int *added, int *is_ansi)
{
	const char *s = ti_get_str(tbl, idx);

	if (!s || !*s) {
		return;
	}

	if (is_ansi && strstr(s, "\x1b[") != NULL) {
		*is_ansi = 1;
	}
	if (added) {
		*added = 1;
	}

	for (size_t i = 0; s[i] != '\0'; i++) {
		if (s[i] == '$' && s[i + 1] == '<') {
			size_t j = i + 2;
			int has_digit = 0;

			while (isdigit((unsigned char)s[j])) {
				has_digit = 1;
				j++;
			}
			if (s[j] == '.') {
				j++;
				while (isdigit((unsigned char)s[j])) {
					has_digit = 1;
					j++;
				}
			}
			while (s[j] == '*' || s[j] == '/') {
				j++;
			}
			if (has_digit && s[j] == '>') {
				i = j;
				continue;
			}
		}
		if (reset_len < sizeof reset_buf) {
			reset_buf[reset_len++] = s[i];
		}
	}
}

void
term_reset_init(const char *term_type, int outfd)
{
	static const char dec_modes[] =
	    "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?2004l";
	static const char vt_fallback[] =
	    "\x1b[?1049l\x1b[?1l\x1b[?25h\x1b[0m";
	struct ti_table tbl;
	unsigned char *ti_buf;
	int have_reset = 0;
	int is_ansi = 0;
	int is_dumb = term_type && strcmp(term_type, "dumb") == 0;

	reset_len = 0;
	reset_ready = 0;
	if (!isatty(outfd) || is_dumb) {
		return;
	}

	ti_buf = load_terminfo(term_type, &tbl);
	if (ti_buf != NULL) {
		append_ti_cap(&tbl, TI_EXIT_CA_MODE, NULL, &is_ansi);
		append_ti_cap(&tbl, TI_KEYPAD_LOCAL, NULL, &is_ansi);
		append_ti_cap(&tbl, TI_EXIT_ALT_CHARSET, NULL, &is_ansi);
		append_ti_cap(&tbl, TI_EXIT_ATTR, NULL, &is_ansi);
		append_ti_cap(&tbl, TI_CURSOR_NORMAL, NULL, &is_ansi);

		append_ti_cap(&tbl, TI_RESET_1, &have_reset, &is_ansi);
		append_ti_cap(&tbl, TI_RESET_2, &have_reset, &is_ansi);
		append_ti_cap(&tbl, TI_RESET_3, &have_reset, &is_ansi);
		if (!have_reset) {
			append_ti_cap(&tbl, TI_INIT_1, NULL, &is_ansi);
			append_ti_cap(&tbl, TI_INIT_2, NULL, &is_ansi);
			append_ti_cap(&tbl, TI_INIT_3, NULL, &is_ansi);
		}
		free(ti_buf);
	}

	if (reset_len == 0) {
		append_raw_bytes(vt_fallback, sizeof vt_fallback - 1);
		is_ansi = 1;
	}

	if (is_ansi) {
		append_raw_bytes(dec_modes, sizeof dec_modes - 1);
	}

	reset_ready = 1;
}

void
term_write_reset(int outfd)
{
	size_t off = 0;

	if (!reset_ready || reset_len == 0) {
		return;
	}

	while (off < reset_len) {
		ssize_t n = write(outfd, reset_buf + off, reset_len - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		off += (size_t)n;
	}
}

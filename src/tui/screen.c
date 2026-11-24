#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tui/screen.h"
#include "utf8.h"

struct tui_cell {
	uint32_t chars[VTERM_MAX_CHARS_PER_CELL];
	uint8_t width;
	struct tui_style st;
};

struct tui_sb {
	char *data;
	size_t len;
	size_t cap;
};

struct tui_surface {
	int rows;
	int cols;
	int outfd;
	struct tui_cell *front;
	struct tui_cell *next;
	struct tui_sb sb;
	int last_cursor_row;
	int last_cursor_col;
	int last_cursor_vis;
	int last_cursor_shape;
};

struct tui_vterm {
	VTerm *vt;
	VTermScreen *vts;
	int rows;
	int cols;
	tui_vterm_reply_fn reply_fn;
	void *reply_user;
	int fwd_fd;
	int dirty;
	VTermPos cursor;
	int cursor_visible;
	int cursor_blink;
	int cursor_shape;
	char title[256];
	int title_dirty;

	/* Small state tracker to mirror keypad/cursor key modes to outer TTY. */
	enum {
		FWD_GROUND = 0,
		FWD_ESC,
		FWD_CSI,
		FWD_CSI_Q,
	} fwd_state;
	char fwd_param[16];
	size_t fwd_plen;
};

static void
sb_reserve(struct tui_sb *sb, size_t extra)
{
	size_t need = sb->len + extra;

	if (need <= sb->cap) {
		return;
	}
	size_t ncap = sb->cap ? sb->cap * 2 : 4096;
	while (ncap < need) {
		ncap *= 2;
	}
	char *nb = realloc(sb->data, ncap);
	if (nb == NULL) {
		perror("realloc");
		exit(EXIT_FAILURE);
	}
	sb->data = nb;
	sb->cap = ncap;
}

static void
sb_append(struct tui_sb *sb, const char *s, size_t n)
{
	if (n == 0) {
		return;
	}
	sb_reserve(sb, n);
	memcpy(sb->data + sb->len, s, n);
	sb->len += n;
}

static void
sb_printf(struct tui_sb *sb, const char *fmt, ...)
{
	va_list ap, ap2;
	int n;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);

	if (n > 0) {
		sb_reserve(sb, (size_t)n + 1);
		vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
		sb->len += (size_t)n;
	}
	va_end(ap2);
}

static void
sb_append_utf8(struct tui_sb *sb, uint32_t cp)
{
	char out[4];
	size_t n = u8_encode(cp, out);

	if (n > 0) {
		sb_append(sb, out, n);
	}
}

static int
write_all_fd(int fd, const char *buf, size_t len)
{
	size_t off = 0;

	if (fd < 0) {
		return 0;
	}
	while (off < len) {
		ssize_t nw = write(fd, buf + off, len - off);

		if (nw < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		off += (size_t)nw;
	}
	return 0;
}

static int
color_equal(const VTermColor *a, const VTermColor *b)
{
	int def_a = VTERM_COLOR_IS_DEFAULT_FG(a) || VTERM_COLOR_IS_DEFAULT_BG(a);
	int def_b = VTERM_COLOR_IS_DEFAULT_FG(b) || VTERM_COLOR_IS_DEFAULT_BG(b);

	if (def_a || def_b) {
		return def_a == def_b &&
		    (a->type & VTERM_COLOR_DEFAULT_MASK) ==
		    (b->type & VTERM_COLOR_DEFAULT_MASK);
	}
	return vterm_color_is_equal(a, b);
}

static int
style_equal(const struct tui_style *a, const struct tui_style *b)
{
	return a->bold == b->bold &&
	    a->italic == b->italic &&
	    a->underline == b->underline &&
	    a->reverse == b->reverse &&
	    a->strike == b->strike &&
	    color_equal(&a->fg, &b->fg) &&
	    color_equal(&a->bg, &b->bg);
}

static int
cell_equal(const struct tui_cell *a, const struct tui_cell *b)
{
	if (a->width != b->width) {
		return 0;
	}
	if (!style_equal(&a->st, &b->st)) {
		return 0;
	}
	for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL; i++) {
		if (a->chars[i] != b->chars[i]) {
			return 0;
		}
		if (a->chars[i] == 0) {
			break;
		}
	}
	return 1;
}

static void
cell_set_blank(struct tui_cell *c, const struct tui_style *st)
{
	memset(c, 0, sizeof *c);
	c->chars[0] = ' ';
	c->width = 1;
	c->st = st ? *st : tui_style_default();
}

static void
append_sgr_color(struct tui_sb *sb, const VTermColor *col, int is_bg)
{
	if ((is_bg && VTERM_COLOR_IS_DEFAULT_BG(col)) ||
	    (!is_bg && VTERM_COLOR_IS_DEFAULT_FG(col))) {
		return;
	}
	if (VTERM_COLOR_IS_INDEXED(col)) {
		uint8_t idx = col->indexed.idx;

		if (idx < 8) {
			sb_printf(sb, ";%d", (is_bg ? 40 : 30) + idx);
		} else if (idx < 16) {
			sb_printf(sb, ";%d", (is_bg ? 100 : 90) + (idx - 8));
		} else {
			sb_printf(sb, ";%d;5;%u", is_bg ? 48 : 38, idx);
		}
	} else if (VTERM_COLOR_IS_RGB(col)) {
		sb_printf(sb, ";%d;2;%u;%u;%u", is_bg ? 48 : 38,
		    col->rgb.red, col->rgb.green, col->rgb.blue);
	}
}

static void
append_sgr(struct tui_sb *sb, const struct tui_style *st)
{
	sb_append(sb, "\x1b[0", 3);
	if (st->bold) {
		sb_append(sb, ";1", 2);
	}
	if (st->italic) {
		sb_append(sb, ";3", 2);
	}
	if (st->underline) {
		sb_append(sb, ";4", 2);
	}
	if (st->reverse) {
		sb_append(sb, ";7", 2);
	}
	if (st->strike) {
		sb_append(sb, ";9", 2);
	}
	append_sgr_color(sb, &st->fg, 0);
	append_sgr_color(sb, &st->bg, 1);
	sb_append(sb, "m", 1);
}

struct tui_surface *
tui_surface_create(int rows, int cols, int outfd)
{
	struct tui_surface *sf = calloc(1, sizeof *sf);

	if (sf == NULL) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	sf->outfd = outfd;
	sf->last_cursor_row = -1;
	sf->last_cursor_col = -1;
	sf->last_cursor_vis = -1;
	sf->last_cursor_shape = -1;
	tui_surface_resize(sf, rows, cols);
	return sf;
}

void
tui_surface_destroy(struct tui_surface *sf)
{
	if (sf == NULL) {
		return;
	}
	free(sf->front);
	free(sf->next);
	free(sf->sb.data);
	free(sf);
}

void
tui_surface_resize(struct tui_surface *sf, int rows, int cols)
{
	size_t n;

	if (rows <= 0) {
		rows = 1;
	}
	if (cols <= 0) {
		cols = 1;
	}
	n = (size_t)rows * (size_t)cols;
	free(sf->front);
	free(sf->next);
	sf->front = calloc(n, sizeof *sf->front);
	sf->next = calloc(n, sizeof *sf->next);
	if (sf->front == NULL || sf->next == NULL) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	sf->rows = rows;
	sf->cols = cols;
	tui_surface_invalidate(sf);
	tui_surface_begin(sf);
}

void
tui_surface_invalidate(struct tui_surface *sf)
{
	size_t n = (size_t)sf->rows * (size_t)sf->cols;

	for (size_t i = 0; i < n; i++) {
		sf->front[i].chars[0] = 0xffffffffu;
		sf->front[i].width = 1;
	}
	sf->last_cursor_row = -1;
	sf->last_cursor_col = -1;
	sf->last_cursor_vis = -1;
	sf->last_cursor_shape = -1;
}

void
tui_surface_begin(struct tui_surface *sf)
{
	size_t n = (size_t)sf->rows * (size_t)sf->cols;

	for (size_t i = 0; i < n; i++) {
		cell_set_blank(&sf->next[i], NULL);
	}
}

void
tui_surface_put_cell(struct tui_surface *sf, int row, int col,
    const VTermScreenCell *vcell)
{
	struct tui_cell *dst;

	if (row < 0 || row >= sf->rows || col < 0 || col >= sf->cols) {
		return;
	}
	dst = &sf->next[(size_t)row * (size_t)sf->cols + (size_t)col];
	memset(dst, 0, sizeof *dst);

	if (vcell->chars[0] == (uint32_t)-1 || vcell->width == 0) {
		dst->width = 0;
		return;
	}

	int w = (vcell->width > 1) ? (int)vcell->width : 1;
	if (col + w > sf->cols) {
		dst->chars[0] = ' ';
		dst->width = 1;
	} else {
		dst->width = (uint8_t)w;
		for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL; i++) {
			dst->chars[i] = vcell->chars[i];
			if (vcell->chars[i] == 0) {
				break;
			}
		}
		if (dst->chars[0] == 0) {
			dst->chars[0] = ' ';
		}
	}

	dst->st.bold = vcell->attrs.bold;
	dst->st.italic = vcell->attrs.italic;
	dst->st.underline = (vcell->attrs.underline != 0);
	dst->st.reverse = vcell->attrs.reverse;
	dst->st.strike = vcell->attrs.strike;
	dst->st.fg = vcell->fg;
	dst->st.bg = vcell->bg;
}

void
tui_surface_put_cp(struct tui_surface *sf, int row, int col, uint32_t cp,
    const struct tui_style *st)
{
	struct tui_cell *dst;

	if (row < 0 || row >= sf->rows || col < 0 || col >= sf->cols) {
		return;
	}
	dst = &sf->next[(size_t)row * (size_t)sf->cols + (size_t)col];
	memset(dst, 0, sizeof *dst);
	dst->chars[0] = cp ? cp : ' ';
	dst->width = 1;
	dst->st = st ? *st : tui_style_default();
}

void
tui_surface_put_str(struct tui_surface *sf, int row, int col, int max_cols,
    const struct tui_style *st, const char *utf8_str)
{
	uint32_t state = 0, cp = 0;
	int c = col;
	int end_col;

	if (row < 0 || row >= sf->rows || utf8_str == NULL || max_cols <= 0) {
		return;
	}
	end_col = col + max_cols;
	if (end_col > sf->cols) {
		end_col = sf->cols;
	}

	for (const unsigned char *p = (const unsigned char *)utf8_str;
	    *p != '\0' && c < end_col; p++) {
		if (!u8_decode(&state, &cp, *p)) {
			if (state != UTF8_ACCEPT) {
				cp = '?';
				state = 0;
			}
			if (c >= 0) {
				tui_surface_put_cp(sf, row, c, cp, st);
			}
			c++;
		}
	}
}

void
tui_surface_fill(struct tui_surface *sf, int row, int col, int nrows,
    int ncols, uint32_t cp, const struct tui_style *st)
{
	for (int r = row; r < row + nrows && r < sf->rows; r++) {
		if (r < 0) {
			continue;
		}
		for (int c = col; c < col + ncols && c < sf->cols; c++) {
			if (c < 0) {
				continue;
			}
			tui_surface_put_cp(sf, r, c, cp, st);
		}
	}
}

int
tui_surface_write_raw(struct tui_surface *sf, const char *bytes, size_t len)
{
	return write_all_fd(sf->outfd, bytes, len);
}

void
tui_surface_flush(struct tui_surface *sf, int cursor_row, int cursor_col,
    int cursor_visible, int cursor_shape)
{
	int any_dirty = 0;
	int phys_r = -1, phys_c = -1;
	int have_st = 0;
	struct tui_style cur_st;

	memset(&cur_st, 0, sizeof cur_st);

	if (cursor_row < 0) {
		cursor_row = 0;
	} else if (cursor_row >= sf->rows) {
		cursor_row = sf->rows - 1;
	}
	if (cursor_col < 0) {
		cursor_col = 0;
	} else if (cursor_col >= sf->cols) {
		cursor_col = sf->cols - 1;
	}

	for (int r = 0; r < sf->rows && !any_dirty; r++) {
		for (int c = 0; c < sf->cols; c++) {
			size_t idx = (size_t)r * (size_t)sf->cols + (size_t)c;

			if (sf->next[idx].width == 0 && c > 0 &&
			    sf->front[idx].width == 0) {
				continue;
			}
			if (!cell_equal(&sf->front[idx], &sf->next[idx])) {
				any_dirty = 1;
				break;
			}
		}
	}

	if (!any_dirty &&
	    sf->last_cursor_row == cursor_row &&
	    sf->last_cursor_col == cursor_col &&
	    sf->last_cursor_vis == cursor_visible &&
	    (cursor_shape <= 0 || sf->last_cursor_shape == cursor_shape)) {
		return;
	}

	sf->sb.len = 0;
	if (any_dirty) {
		sb_append(&sf->sb, "\x1b[?25l", 6);
		for (int r = 0; r < sf->rows; r++) {
			for (int c = 0; c < sf->cols; c++) {
				size_t idx = (size_t)r * (size_t)sf->cols + (size_t)c;
				const struct tui_cell *nc = &sf->next[idx];

				if (nc->width == 0) {
					sf->front[idx] = *nc;
					continue;
				}
				if (cell_equal(&sf->front[idx], nc)) {
					continue;
				}

				if (phys_r != r || phys_c != c) {
					sb_printf(&sf->sb, "\x1b[%d;%dH",
					    r + 1, c + 1);
					phys_r = r;
					phys_c = c;
				}
				if (!have_st || !style_equal(&cur_st, &nc->st)) {
					append_sgr(&sf->sb, &nc->st);
					cur_st = nc->st;
					have_st = 1;
				}
				for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL; i++) {
					if (nc->chars[i] == 0) {
						break;
					}
					sb_append_utf8(&sf->sb, nc->chars[i]);
				}
				sf->front[idx] = *nc;
				phys_c += nc->width;
				if (phys_c >= sf->cols) {
					phys_r = -1;
					phys_c = -1;
				}
			}
		}
		sb_append(&sf->sb, "\x1b[0m", 4);
	}

	if (cursor_shape > 0 && cursor_shape != sf->last_cursor_shape) {
		sb_printf(&sf->sb, "\x1b[%d q", cursor_shape);
		sf->last_cursor_shape = cursor_shape;
	}
	if (any_dirty || sf->last_cursor_row != cursor_row ||
	    sf->last_cursor_col != cursor_col) {
		sb_printf(&sf->sb, "\x1b[%d;%dH", cursor_row + 1,
		    cursor_col + 1);
		sf->last_cursor_row = cursor_row;
		sf->last_cursor_col = cursor_col;
	}
	if (any_dirty || sf->last_cursor_vis != cursor_visible) {
		sb_append(&sf->sb, cursor_visible ? "\x1b[?25h" : "\x1b[?25l", 6);
		sf->last_cursor_vis = cursor_visible;
	}

	if (sf->sb.len > 0) {
		(void)write_all_fd(sf->outfd, sf->sb.data, sf->sb.len);
	}
}

static int
cb_damage(VTermRect rect, void *user)
{
	struct tui_vterm *tv = user;

	(void)rect;
	tv->dirty = 1;
	return 1;
}

static int
cb_moverect(VTermRect dest, VTermRect src, void *user)
{
	struct tui_vterm *tv = user;

	(void)dest;
	(void)src;
	tv->dirty = 1;
	return 1;
}

static int
cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
	struct tui_vterm *tv = user;

	(void)oldpos;
	tv->cursor = pos;
	tv->cursor_visible = visible;
	tv->dirty = 1;
	return 1;
}

static int
cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
	struct tui_vterm *tv = user;

	switch (prop) {
	case VTERM_PROP_CURSORVISIBLE:
		tv->cursor_visible = val->boolean ? 1 : 0;
		tv->dirty = 1;
		return 1;
	case VTERM_PROP_CURSORBLINK:
		tv->cursor_blink = val->boolean ? 1 : 0;
		tv->dirty = 1;
		return 1;
	case VTERM_PROP_CURSORSHAPE:
		tv->cursor_shape = val->number;
		tv->dirty = 1;
		return 1;
	case VTERM_PROP_ALTSCREEN:
		tv->dirty = 1;
		return 1;
	case VTERM_PROP_TITLE:
		if (val->string.initial) {
			tv->title[0] = '\0';
		}
		if (val->string.str != NULL && val->string.len > 0) {
			size_t cur = strlen(tv->title);
			size_t rem = sizeof tv->title - 1 - cur;
			size_t take = val->string.len < rem ?
			    val->string.len : rem;

			if (take > 0) {
				memcpy(tv->title + cur, val->string.str, take);
				tv->title[cur + take] = '\0';
			}
		}
		if (val->string.final) {
			tv->title_dirty = 1;
		}
		return 1;
	case VTERM_PROP_MOUSE:
		if (tv->fwd_fd >= 0) {
			const char *seq = NULL;

			if (val->number == VTERM_PROP_MOUSE_NONE) {
				seq = "\x1b[?1000l\x1b[?1002l\x1b[?1003l";
			} else if (val->number == VTERM_PROP_MOUSE_CLICK) {
				seq = "\x1b[?1000h";
			} else if (val->number == VTERM_PROP_MOUSE_DRAG) {
				seq = "\x1b[?1002h";
			} else if (val->number == VTERM_PROP_MOUSE_MOVE) {
				seq = "\x1b[?1003h";
			}
			if (seq != NULL) {
				(void)write_all_fd(tv->fwd_fd, seq, strlen(seq));
			}
		}
		return 1;
	case VTERM_PROP_FOCUSREPORT:
		if (tv->fwd_fd >= 0) {
			const char *seq = val->boolean ?
			    "\x1b[?1004h" : "\x1b[?1004l";

			(void)write_all_fd(tv->fwd_fd, seq, strlen(seq));
		}
		return 1;
	default:
		return 0;
	}
}

static int
cb_resize(int rows, int cols, void *user)
{
	struct tui_vterm *tv = user;

	tv->rows = rows;
	tv->cols = cols;
	tv->dirty = 1;
	return 1;
}

static int
cb_unrecognised_csi(const char *leader, const long args[], int argcount,
    const char *intermed, char command, void *user)
{
	struct tui_vterm *tv = user;

	if (tv->fwd_fd < 0 || leader == NULL || strcmp(leader, "?") != 0 ||
	    (intermed != NULL && *intermed != '\0') ||
	    (command != 'h' && command != 'l') || argcount <= 0) {
		return 0;
	}

	/* Forward private mouse SGR (1006) & bracketed paste (2004) cleanly. */
	for (int i = 0; i < argcount; i++) {
		long code = CSI_ARG(args[i]);

		if (code == 1006 || code == 2004) {
			char seq[32];
			int n = snprintf(seq, sizeof seq, "\x1b[?%ld%c",
			    code, command);

			if (n > 0 && (size_t)n < sizeof seq) {
				(void)write_all_fd(tv->fwd_fd, seq, (size_t)n);
			}
		}
	}
	return 1;
}

static const VTermScreenCallbacks vts_callbacks = {
	.damage = cb_damage,
	.moverect = cb_moverect,
	.movecursor = cb_movecursor,
	.settermprop = cb_settermprop,
	.resize = cb_resize,
};

static const VTermStateFallbacks vts_fallbacks = {
	.csi = cb_unrecognised_csi,
};

static void
on_vt_output(const char *bytes, size_t len, void *user)
{
	struct tui_vterm *tv = user;

	if (tv->reply_fn != NULL && len > 0) {
		tv->reply_fn(bytes, len, tv->reply_user);
	}
}

/* Forward DECCKM (?1h/?1l) and keypad (ESC=/ESC>) across read boundaries so
 * interactive fullscreen tools inside PTY receive matching key sequences. */
static void
scan_input_modes(struct tui_vterm *tv, const char *bytes, size_t len,
    int outfd)
{
	if (outfd < 0) {
		return;
	}
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)bytes[i];

		switch (tv->fwd_state) {
		case FWD_GROUND:
			if (c == 0x1b) {
				tv->fwd_state = FWD_ESC;
			}
			break;
		case FWD_ESC:
			if (c == '[') {
				tv->fwd_state = FWD_CSI;
			} else if (c == '=' || c == '>') {
				char seq[2] = { 0x1b, (char)c };

				(void)write_all_fd(outfd, seq, 2);
				tv->fwd_state = FWD_GROUND;
			} else {
				tv->fwd_state = (c == 0x1b) ? FWD_ESC : FWD_GROUND;
			}
			break;
		case FWD_CSI:
			if (c == '?') {
				tv->fwd_state = FWD_CSI_Q;
				tv->fwd_plen = 0;
			} else {
				tv->fwd_state = (c == 0x1b) ? FWD_ESC : FWD_GROUND;
			}
			break;
		case FWD_CSI_Q:
			if (c >= '0' && c <= '9') {
				if (tv->fwd_plen + 1 < sizeof tv->fwd_param) {
					tv->fwd_param[tv->fwd_plen++] = (char)c;
				}
			} else if (c == 'h' || c == 'l') {
				tv->fwd_param[tv->fwd_plen] = '\0';
				if (strcmp(tv->fwd_param, "1") == 0) {
					char seq[6] = {
						0x1b, '[', '?', '1', (char)c
					};

					(void)write_all_fd(outfd, seq, 5);
				}
				tv->fwd_state = FWD_GROUND;
			} else {
				tv->fwd_state = (c == 0x1b) ? FWD_ESC : FWD_GROUND;
			}
			break;
		}
	}
}

struct tui_vterm *
tui_vterm_create(int rows, int cols, tui_vterm_reply_fn reply_fn,
    void *reply_user)
{
	struct tui_vterm *tv = calloc(1, sizeof *tv);

	if (rows <= 0) {
		rows = 1;
	}
	if (cols <= 0) {
		cols = 1;
	}
	if (tv == NULL) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}

	tv->vt = vterm_new(rows, cols);
	if (tv->vt == NULL) {
		fprintf(stderr, "vterm_new failed\n");
		exit(EXIT_FAILURE);
	}
	vterm_set_utf8(tv->vt, 1);

	tv->rows = rows;
	tv->cols = cols;
	tv->reply_fn = reply_fn;
	tv->reply_user = reply_user;
	tv->fwd_fd = -1;
	tv->cursor_visible = 1;
	tv->cursor_blink = 1;
	tv->dirty = 1;

	vterm_output_set_callback(tv->vt, on_vt_output, tv);

	tv->vts = vterm_obtain_screen(tv->vt);
	vterm_screen_enable_altscreen(tv->vts, 1);
	vterm_screen_enable_reflow(tv->vts, true);
	vterm_screen_set_damage_merge(tv->vts, VTERM_DAMAGE_SCROLL);
	vterm_screen_set_callbacks(tv->vts, &vts_callbacks, tv);
	vterm_screen_set_unrecognised_fallbacks(tv->vts, &vts_fallbacks, tv);
	vterm_screen_reset(tv->vts, 1);
	return tv;
}

void
tui_vterm_destroy(struct tui_vterm *tv)
{
	if (tv == NULL) {
		return;
	}
	if (tv->vt != NULL) {
		vterm_free(tv->vt);
	}
	free(tv);
}

void
tui_vterm_resize(struct tui_vterm *tv, int rows, int cols)
{
	if (rows <= 0) {
		rows = 1;
	}
	if (cols <= 0) {
		cols = 1;
	}
	if (tv->rows == rows && tv->cols == cols) {
		return;
	}
	vterm_set_size(tv->vt, rows, cols);
	vterm_screen_flush_damage(tv->vts);
	tv->rows = rows;
	tv->cols = cols;
	tv->dirty = 1;
}

void
tui_vterm_feed(struct tui_vterm *tv, const char *bytes, size_t len, int fwd_fd)
{
	if (len == 0) {
		return;
	}
	tv->fwd_fd = fwd_fd;
	scan_input_modes(tv, bytes, len, fwd_fd);
	vterm_input_write(tv->vt, bytes, len);
	vterm_screen_flush_damage(tv->vts);
}

void
tui_vterm_blit(struct tui_vterm *tv, struct tui_surface *sf, int dst_row,
    int dst_col)
{
	for (int r = 0; r < tv->rows; r++) {
		if (dst_row + r >= sf->rows) {
			break;
		}
		for (int c = 0; c < tv->cols; c++) {
			VTermScreenCell cell;
			VTermPos pos = { .row = r, .col = c };

			if (dst_col + c >= sf->cols) {
				break;
			}
			if (vterm_screen_get_cell(tv->vts, pos, &cell)) {
				tui_surface_put_cell(sf, dst_row + r,
				    dst_col + c, &cell);
			}
		}
	}
}

int
tui_vterm_take_dirty(struct tui_vterm *tv)
{
	int d = tv->dirty;

	tv->dirty = 0;
	return d;
}

void
tui_vterm_get_cursor(const struct tui_vterm *tv, int *row, int *col,
    int *visible, int *shape)
{
	if (row != NULL) {
		*row = tv->cursor.row;
	}
	if (col != NULL) {
		*col = tv->cursor.col;
	}
	if (visible != NULL) {
		*visible = tv->cursor_visible;
	}
	if (shape != NULL) {
		*shape = tui_vterm_decscusr(tv->cursor_shape,
		    tv->cursor_blink);
	}
}

int
tui_vterm_take_title(struct tui_vterm *tv, char *buf, size_t sz)
{
	if (!tv->title_dirty) {
		return 0;
	}
	tv->title_dirty = 0;
	if (buf != NULL && sz > 0) {
		snprintf(buf, sz, "%s", tv->title);
	}
	return 1;
}

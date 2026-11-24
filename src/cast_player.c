#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cast_player.h"
#include "utf8.h"

enum {
	CAST_CKPT_EVENTS = 256,
};

struct cast_ckpt {
	size_t ev_idx;
	double t_ms;
	int cols;
	int rows;
	char *ansi;
	size_t len;
};

struct cast_player {
	const struct cast_doc *doc;
	VTerm *vt;
	VTermScreen *vts;
	int cols;
	int rows;
	int altscreen;
	VTermPos cursor;
	int cursor_visible;
	int cursor_blink;
	int cursor_shape;

	size_t next_ev;
	double cur_ms;

	struct cast_ckpt *ckpts;
	size_t nckpts;
	size_t ccap;
};

struct ansi_sb {
	char *data;
	size_t len;
	size_t cap;
};

static void
asb_reserve(struct ansi_sb *b, size_t extra)
{
	size_t need = b->len + extra;

	if (need <= b->cap) {
		return;
	}
	size_t ncap = b->cap ? b->cap * 2 : 1024;
	while (ncap < need) {
		ncap *= 2;
	}
	char *np = realloc(b->data, ncap);
	if (np == NULL) {
		perror("realloc");
		exit(EXIT_FAILURE);
	}
	b->data = np;
	b->cap = ncap;
}

static void
asb_append(struct ansi_sb *b, const char *s, size_t n)
{
	if (n == 0) {
		return;
	}
	asb_reserve(b, n);
	memcpy(b->data + b->len, s, n);
	b->len += n;
}

static void
asb_printf(struct ansi_sb *b, const char *fmt, ...)
{
	va_list ap, ap2;
	int n;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);

	if (n > 0) {
		asb_reserve(b, (size_t)n + 1);
		vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
		b->len += (size_t)n;
	}
	va_end(ap2);
}

static void
asb_utf8(struct ansi_sb *b, uint32_t cp)
{
	char out[4];
	size_t n = u8_encode(cp, out);

	if (n > 0) {
		asb_append(b, out, n);
	}
}

static int
pl_cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
	struct cast_player *pl = user;

	(void)oldpos;
	pl->cursor = pos;
	pl->cursor_visible = visible;
	return 1;
}

static int
pl_cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
	struct cast_player *pl = user;

	switch (prop) {
	case VTERM_PROP_CURSORVISIBLE:
		pl->cursor_visible = val->boolean ? 1 : 0;
		return 1;
	case VTERM_PROP_CURSORBLINK:
		pl->cursor_blink = val->boolean ? 1 : 0;
		return 1;
	case VTERM_PROP_CURSORSHAPE:
		pl->cursor_shape = val->number;
		return 1;
	case VTERM_PROP_ALTSCREEN:
		pl->altscreen = val->boolean ? 1 : 0;
		return 1;
	case VTERM_PROP_REVERSE:
		return 1;
	default:
		return 0;
	}
}

static const VTermScreenCallbacks pl_vts_cbs = {
	.movecursor = pl_cb_movecursor,
	.settermprop = pl_cb_settermprop,
};

static void
append_cell_sgr(struct ansi_sb *b, const VTermScreenCell *c)
{
	asb_append(b, "\x1b[0", 3);
	if (c->attrs.bold) {
		asb_append(b, ";1", 2);
	}
	if (c->attrs.italic) {
		asb_append(b, ";3", 2);
	}
	if (c->attrs.underline) {
		asb_append(b, ";4", 2);
	}
	if (c->attrs.reverse) {
		asb_append(b, ";7", 2);
	}
	if (c->attrs.strike) {
		asb_append(b, ";9", 2);
	}
	if (!VTERM_COLOR_IS_DEFAULT_FG(&c->fg)) {
		if (VTERM_COLOR_IS_INDEXED(&c->fg)) {
			uint8_t idx = c->fg.indexed.idx;

			if (idx < 8) {
				asb_printf(b, ";%d", 30 + idx);
			} else if (idx < 16) {
				asb_printf(b, ";%d", 90 + (idx - 8));
			} else {
				asb_printf(b, ";38;5;%u", idx);
			}
		} else if (VTERM_COLOR_IS_RGB(&c->fg)) {
			asb_printf(b, ";38;2;%u;%u;%u",
			    c->fg.rgb.red, c->fg.rgb.green, c->fg.rgb.blue);
		}
	}
	if (!VTERM_COLOR_IS_DEFAULT_BG(&c->bg)) {
		if (VTERM_COLOR_IS_INDEXED(&c->bg)) {
			uint8_t idx = c->bg.indexed.idx;

			if (idx < 8) {
				asb_printf(b, ";%d", 40 + idx);
			} else if (idx < 16) {
				asb_printf(b, ";%d", 100 + (idx - 8));
			} else {
				asb_printf(b, ";48;5;%u", idx);
			}
		} else if (VTERM_COLOR_IS_RGB(&c->bg)) {
			asb_printf(b, ";48;2;%u;%u;%u",
			    c->bg.rgb.red, c->bg.rgb.green, c->bg.rgb.blue);
		}
	}
	asb_append(b, "m", 1);
}

static int
cell_is_plain_blank(const VTermScreenCell *c)
{
	return (c->chars[0] == 0 || c->chars[0] == ' ') &&
	    !c->attrs.bold && !c->attrs.italic && !c->attrs.underline &&
	    !c->attrs.reverse && !c->attrs.strike &&
	    VTERM_COLOR_IS_DEFAULT_FG(&c->fg) &&
	    VTERM_COLOR_IS_DEFAULT_BG(&c->bg);
}

static int
cell_same_style(const VTermScreenCell *a, const VTermScreenCell *b)
{
	int dfg_a = VTERM_COLOR_IS_DEFAULT_FG(&a->fg);
	int dfg_b = VTERM_COLOR_IS_DEFAULT_FG(&b->fg);
	int dbg_a = VTERM_COLOR_IS_DEFAULT_BG(&a->bg);
	int dbg_b = VTERM_COLOR_IS_DEFAULT_BG(&b->bg);

	if (a->attrs.bold != b->attrs.bold ||
	    a->attrs.italic != b->attrs.italic ||
	    (a->attrs.underline != 0) != (b->attrs.underline != 0) ||
	    a->attrs.reverse != b->attrs.reverse ||
	    a->attrs.strike != b->attrs.strike) {
		return 0;
	}
	if (dfg_a != dfg_b || (!dfg_a && !vterm_color_is_equal(&a->fg, &b->fg))) {
		return 0;
	}
	if (dbg_a != dbg_b || (!dbg_a && !vterm_color_is_equal(&a->bg, &b->bg))) {
		return 0;
	}
	return 1;
}

static void
append_screen_grid(const struct cast_player *pl, struct ansi_sb *b)
{
	int dec_shape = tui_vterm_decscusr(pl->cursor_shape, pl->cursor_blink);

	asb_append(b, "\x1b[0m\x1b[2J\x1b[H", 11);

	for (int r = 0; r < pl->rows; r++) {
		int last_col = -1;

		for (int c = pl->cols - 1; c >= 0; c--) {
			VTermScreenCell cell;
			VTermPos pos = { .row = r, .col = c };

			if (!vterm_screen_get_cell(pl->vts, pos, &cell)) {
				continue;
			}
			if (cell.chars[0] == (uint32_t)-1 || cell.width == 0 ||
			    !cell_is_plain_blank(&cell)) {
				last_col = c;
				break;
			}
		}
		if (last_col < 0) {
			continue;
		}
		asb_printf(b, "\x1b[%d;1H", r + 1);
		VTermScreenCell cur_st;
		int have_st = 0;

		for (int c = 0; c <= last_col; c++) {
			VTermScreenCell cell;
			VTermPos pos = { .row = r, .col = c };

			if (!vterm_screen_get_cell(pl->vts, pos, &cell)) {
				continue;
			}
			if (cell.chars[0] == (uint32_t)-1 || cell.width == 0) {
				continue;
			}
			if (!have_st || !cell_same_style(&cur_st, &cell)) {
				append_cell_sgr(b, &cell);
				cur_st = cell;
				have_st = 1;
			}
			if (cell.chars[0] == 0) {
				asb_append(b, " ", 1);
			} else {
				for (int k = 0; k < VTERM_MAX_CHARS_PER_CELL; k++) {
					if (cell.chars[k] == 0) {
						break;
					}
					asb_utf8(b, cell.chars[k]);
				}
			}
		}
	}

	asb_printf(b, "\x1b[0m\x1b[%d q\x1b[%d;%dH%s", dec_shape,
	    pl->cursor.row + 1, pl->cursor.col + 1,
	    pl->cursor_visible ? "\x1b[?25h" : "\x1b[?25l");
}

static char *
serialize_main_ckpt(const struct cast_player *pl, size_t *len_out)
{
	struct ansi_sb b = { 0 };

	asb_append(&b, "\x1b[?1049l", 8);
	append_screen_grid(pl, &b);
	if (len_out != NULL) {
		*len_out = b.len;
	}
	return b.data;
}

static void
reset_vterm_for_doc(struct cast_player *pl)
{
	int cols = pl->doc->cols > 0 ? pl->doc->cols : 80;
	int rows = pl->doc->rows > 0 ? pl->doc->rows : 24;

	if (pl->vt != NULL) {
		vterm_free(pl->vt);
	}
	pl->vt = vterm_new(rows, cols);
	if (pl->vt == NULL) {
		perror("vterm_new");
		exit(EXIT_FAILURE);
	}
	vterm_set_utf8(pl->vt, 1);
	pl->vts = vterm_obtain_screen(pl->vt);
	vterm_screen_enable_altscreen(pl->vts, 1);
	vterm_screen_enable_reflow(pl->vts, true);
	vterm_screen_set_damage_merge(pl->vts, VTERM_DAMAGE_SCROLL);
	vterm_screen_set_callbacks(pl->vts, &pl_vts_cbs, pl);
	vterm_screen_reset(pl->vts, 1);

	pl->cols = cols;
	pl->rows = rows;
	pl->altscreen = 0;
	pl->cursor = (VTermPos){ 0, 0 };
	pl->cursor_visible = 1;
	pl->cursor_blink = 1;
	pl->cursor_shape = 0;
	pl->next_ev = 0;
	pl->cur_ms = 0.0;
}

static void
free_ckpts(struct cast_player *pl)
{
	for (size_t i = 0; i < pl->nckpts; i++) {
		free(pl->ckpts[i].ansi);
	}
	free(pl->ckpts);
	pl->ckpts = NULL;
	pl->nckpts = 0;
	pl->ccap = 0;
}

static void
maybe_record_ckpt(struct cast_player *pl, size_t ev_idx, double t_ms)
{
	size_t prev_ev = (pl->nckpts > 0) ?
	    pl->ckpts[pl->nckpts - 1].ev_idx : 0;

	/* Defer checkpoints while inside altscreen so main screen isn't lost. */
	if (pl->altscreen || ev_idx < prev_ev + CAST_CKPT_EVENTS) {
		return;
	}
	if (pl->nckpts == pl->ccap) {
		size_t nc = pl->ccap ? pl->ccap * 2 : 16;
		struct cast_ckpt *nk = realloc(pl->ckpts, nc * sizeof *nk);

		if (nk == NULL) {
			return;
		}
		pl->ckpts = nk;
		pl->ccap = nc;
	}
	size_t slen = 0;
	char *ansi = serialize_main_ckpt(pl, &slen);

	if (ansi == NULL) {
		return;
	}
	pl->ckpts[pl->nckpts++] = (struct cast_ckpt){
		.ev_idx = ev_idx,
		.t_ms = t_ms,
		.cols = pl->cols,
		.rows = pl->rows,
		.ansi = ansi,
		.len = slen,
	};
}

static void
apply_event(struct cast_player *pl, const struct cast_event *ev)
{
	if (ev->type == CAST_EV_OUT && ev->len > 0) {
		vterm_input_write(pl->vt, ev->data, ev->len);
		vterm_screen_flush_damage(pl->vts);
	} else if (ev->type == CAST_EV_RESIZE && ev->cols > 0 && ev->rows > 0) {
		if (ev->cols != pl->cols || ev->rows != pl->rows) {
			vterm_set_size(pl->vt, ev->rows, ev->cols);
			vterm_screen_flush_damage(pl->vts);
			pl->cols = ev->cols;
			pl->rows = ev->rows;
		}
	}
}

struct cast_player *
cast_player_create(const struct cast_doc *doc)
{
	struct cast_player *pl = calloc(1, sizeof *pl);

	if (pl == NULL) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	pl->doc = doc;
	reset_vterm_for_doc(pl);
	return pl;
}

void
cast_player_destroy(struct cast_player *pl)
{
	if (pl == NULL) {
		return;
	}
	free_ckpts(pl);
	if (pl->vt != NULL) {
		vterm_free(pl->vt);
	}
	free(pl);
}

void
cast_player_invalidate(struct cast_player *pl)
{
	free_ckpts(pl);
	reset_vterm_for_doc(pl);
}

int
cast_player_seek(struct cast_player *pl, double target_ms)
{
	if (target_ms < 0.0) {
		target_ms = 0.0;
	}
	if (target_ms > pl->doc->duration_ms) {
		target_ms = pl->doc->duration_ms;
	}

	if (target_ms < pl->cur_ms ||
	    (pl->next_ev > 0 && pl->doc->events[pl->next_ev - 1].t_ms > target_ms)) {
		const struct cast_ckpt *best = NULL;

		for (size_t i = 0; i < pl->nckpts; i++) {
			if (pl->ckpts[i].t_ms <= target_ms) {
				best = &pl->ckpts[i];
			} else {
				break;
			}
		}

		reset_vterm_for_doc(pl);
		if (best != NULL) {
			if (best->cols != pl->cols || best->rows != pl->rows) {
				vterm_set_size(pl->vt, best->rows, best->cols);
				pl->cols = best->cols;
				pl->rows = best->rows;
			}
			if (best->len > 0) {
				vterm_input_write(pl->vt, best->ansi, best->len);
				vterm_screen_flush_damage(pl->vts);
			}
			pl->next_ev = best->ev_idx;
		}
	}

	while (pl->next_ev < pl->doc->nevents &&
	    pl->doc->events[pl->next_ev].t_ms <= target_ms) {
		apply_event(pl, &pl->doc->events[pl->next_ev]);
		pl->next_ev++;
		maybe_record_ckpt(pl, pl->next_ev,
		    pl->doc->events[pl->next_ev - 1].t_ms);
	}

	pl->cur_ms = target_ms;
	return 0;
}

int
cast_player_cols(const struct cast_player *pl)
{
	return pl->cols;
}

int
cast_player_rows(const struct cast_player *pl)
{
	return pl->rows;
}

double
cast_player_pos_ms(const struct cast_player *pl)
{
	return pl->cur_ms;
}

void
cast_player_blit(const struct cast_player *pl, struct tui_surface *sf,
    int src_row, int src_col, int dst_row, int dst_col, int max_rows,
    int max_cols)
{
	int avail_r = pl->rows - src_row;
	int avail_c = pl->cols - src_col;
	int nr, nc;

	if (src_row < 0 || src_col < 0 || avail_r <= 0 || avail_c <= 0) {
		return;
	}
	nr = avail_r < max_rows ? avail_r : max_rows;
	nc = avail_c < max_cols ? avail_c : max_cols;

	for (int r = 0; r < nr; r++) {
		for (int c = 0; c < nc; c++) {
			VTermScreenCell cell;
			VTermPos pos = {
				.row = src_row + r,
				.col = src_col + c,
			};

			if (vterm_screen_get_cell(pl->vts, pos, &cell)) {
				if ((c == 0 && cell.width == 0) ||
				    (cell.width > 1 && c + cell.width > nc)) {
					cell.chars[0] = ' ';
					cell.width = 1;
				}
				tui_surface_put_cell(sf, dst_row + r,
				    dst_col + c, &cell);
			}
		}
	}
}

void
cast_player_get_cursor(const struct cast_player *pl, int *row, int *col,
    int *visible, int *shape)
{
	if (row != NULL) {
		*row = pl->cursor.row;
	}
	if (col != NULL) {
		*col = pl->cursor.col;
	}
	if (visible != NULL) {
		*visible = pl->cursor_visible;
	}
	if (shape != NULL) {
		*shape = tui_vterm_decscusr(pl->cursor_shape, pl->cursor_blink);
	}
}

int
cast_player_snapshot_ansi(const struct cast_doc *doc, double at_ms,
    char **out_buf, size_t *out_len, int *cols_out, int *rows_out)
{
	struct cast_player *pl = cast_player_create(doc);
	struct ansi_sb out = { 0 };

	cast_player_seek(pl, at_ms);
	if (cols_out != NULL) {
		*cols_out = pl->cols;
	}
	if (rows_out != NULL) {
		*rows_out = pl->rows;
	}

	if (pl->altscreen) {
		struct ansi_sb alt_grid = { 0 };

		append_screen_grid(pl, &alt_grid);
		/* Switch back to main screen to capture underlying shell state. */
		vterm_input_write(pl->vt, "\x1b[?1049l", 8);
		vterm_screen_flush_damage(pl->vts);

		asb_append(&out, "\x1b[?1049l", 8);
		append_screen_grid(pl, &out);
		asb_append(&out, "\x1b[?1049h", 8);
		asb_append(&out, alt_grid.data, alt_grid.len);
		free(alt_grid.data);
	} else {
		asb_append(&out, "\x1b[?1049l", 8);
		append_screen_grid(pl, &out);
	}

	cast_player_destroy(pl);
	*out_buf = out.data;
	if (out_len != NULL) {
		*out_len = out.len;
	}
	return (*out_buf != NULL) ? 0 : -1;
}

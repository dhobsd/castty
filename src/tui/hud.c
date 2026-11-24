#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "tui/hud.h"

int
hud_parse_mode(const char *arg, enum hud_mode *out)
{
	if (arg == NULL || out == NULL) {
		return -1;
	}
	if (strcasecmp(arg, "auto") == 0) {
		*out = HUD_AUTO;
		return 0;
	}
	if (strcasecmp(arg, "side") == 0) {
		*out = HUD_SIDE;
		return 0;
	}
	if (strcasecmp(arg, "bottom") == 0) {
		*out = HUD_BOTTOM;
		return 0;
	}
	if (strcasecmp(arg, "off") == 0 || strcasecmp(arg, "none") == 0) {
		*out = HUD_OFF;
		return 0;
	}
	return -1;
}

static int
clamp_pos(int req, int limit)
{
	if (limit <= 0) {
		return 1;
	}
	if (req <= 0 || req > limit) {
		return limit;
	}
	return req;
}

/* Pure async-signal-safe layout computation shared between parent SIGWINCH
 * sizing and child compositor rendering. */
void
hud_compute_layout(enum hud_mode req_mode, int outer_rows, int outer_cols,
    int req_rows, int req_cols, int hidden, struct hud_layout *out)
{
	enum hud_mode chosen = HUD_OFF;
	int r_cap = outer_rows > 0 ? outer_rows : 24;
	int c_cap = outer_cols > 0 ? outer_cols : 80;
	int cr, cc;

	memset(out, 0, sizeof *out);
	out->outer_rows = r_cap;
	out->outer_cols = c_cap;

	if (!hidden && req_mode != HUD_OFF) {
		if (req_mode == HUD_SIDE) {
			if (c_cap >= HUD_SIDE_COLS + 16) {
				chosen = HUD_SIDE;
			} else if (r_cap >= 4) {
				chosen = HUD_BOTTOM;
			}
		} else if (req_mode == HUD_BOTTOM) {
			if (r_cap >= 4) {
				chosen = HUD_BOTTOM;
			}
		} else {
			/* HUD_AUTO: prefer side column without shrinking below 80
			 * cols (or without shrinking explicit -c if headroom exists),
			 * else 1-row bottom bar when height allows. */
			if (req_cols > 0) {
				if (req_cols + HUD_SIDE_COLS <= c_cap) {
					chosen = HUD_SIDE;
				} else if (req_rows == 0 && r_cap >= 12) {
					chosen = HUD_BOTTOM;
				} else if (req_rows > 0 &&
				    req_rows + HUD_BOTTOM_ROWS <= r_cap) {
					chosen = HUD_BOTTOM;
				}
			} else if (c_cap >= 80 + HUD_SIDE_COLS) {
				chosen = HUD_SIDE;
			} else if (r_cap >= 12 && c_cap >= 40) {
				chosen = HUD_BOTTOM;
			}
		}
	}

	out->active_mode = chosen;
	if (chosen == HUD_SIDE) {
		cr = clamp_pos(req_rows, r_cap);
		cc = clamp_pos(req_cols, c_cap - HUD_SIDE_COLS);
		out->child_rows = cr;
		out->child_cols = cc;
		out->hud_row = 0;
		out->hud_col = c_cap - HUD_SIDE_COLS;
		out->hud_rows = r_cap;
		out->hud_cols = HUD_SIDE_COLS;
	} else if (chosen == HUD_BOTTOM) {
		cr = clamp_pos(req_rows, r_cap - HUD_BOTTOM_ROWS);
		cc = clamp_pos(req_cols, c_cap);
		out->child_rows = cr;
		out->child_cols = cc;
		out->hud_row = r_cap - HUD_BOTTOM_ROWS;
		out->hud_col = 0;
		out->hud_rows = HUD_BOTTOM_ROWS;
		out->hud_cols = c_cap;
	} else {
		out->child_rows = clamp_pos(req_rows, r_cap);
		out->child_cols = clamp_pos(req_cols, c_cap);
	}
}

void
hud_init(struct hud_state *st, int paused, int append, int audio_enabled,
    const char *audio_fmt, int initial_markers, double initial_ms)
{
	memset(st, 0, sizeof *st);
	st->paused = paused;
	st->append = append;
	st->audio_enabled = audio_enabled;
	st->audio_fmt = audio_fmt ? audio_fmt : "none";
	st->markers = initial_markers;
	st->elapsed_ms = initial_ms;
	st->peak_db = -60.0f;
	st->tl_last_step = (initial_ms > 0.0) ? (uint64_t)(initial_ms / 500.0) : 0;
}

void
hud_note_output(struct hud_state *st)
{
	st->cur_tl_flags |= HUD_TL_TERM;
}

void
hud_note_marker_label(struct hud_state *st, double at_ms, const char *label)
{
	st->markers++;
	st->last_marker_ms = at_ms;
	st->cur_tl_flags |= HUD_TL_MARK;
	st->mark_flash_ttl = 24;
	if (label != NULL) {
		snprintf(st->last_marker_label, sizeof st->last_marker_label,
		    "%s", label);
	} else {
		st->last_marker_label[0] = '\0';
	}
}

void
hud_note_marker(struct hud_state *st, double at_ms)
{
	hud_note_marker_label(st, at_ms, NULL);
}

void
hud_set_prompt(struct hud_state *st, int active, const char *text)
{
	st->prompt_active = active ? 1 : 0;
	if (active && text != NULL) {
		snprintf(st->prompt_buf, sizeof st->prompt_buf, "%s", text);
	} else {
		st->prompt_buf[0] = '\0';
	}
}

void
hud_note_pause(struct hud_state *st, int paused)
{
	st->paused = paused;
	if (paused) {
		st->vu_l = 0.0f;
		st->vu_r = 0.0f;
	}
}

static float
amp_to_norm(float v)
{
	float db, norm;

	if (v <= 0.001f) {
		return 0.0f;
	}
	db = 20.0f * log10f(v);
	norm = (db + 60.0f) / 60.0f;
	if (norm < 0.0f) {
		return 0.0f;
	}
	if (norm > 1.0f) {
		return 1.0f;
	}
	return norm;
}

static void
update_channel_vu(float target, float *lvl, float *hold, int *hold_ttl)
{
	if (target >= *lvl) {
		*lvl = target;
	} else {
		*lvl -= 0.025f;
		if (*lvl < 0.0f) {
			*lvl = 0.0f;
		}
	}

	if (*lvl >= *hold) {
		*hold = *lvl;
		*hold_ttl = 20;
	} else if (*hold_ttl > 0) {
		(*hold_ttl)--;
	} else {
		*hold -= 0.015f;
		if (*hold < *lvl) {
			*hold = *lvl;
		}
	}
}

void
hud_tick(struct hud_state *st, double elapsed_ms, const struct audio_vu *vu)
{
	float in_l = 0.0f, in_r = 0.0f, raw_peak = 0.0f;
	uint64_t step;

	st->elapsed_ms = elapsed_ms;
	if (st->mark_flash_ttl > 0) {
		st->mark_flash_ttl--;
	}
	if (st->clip_ttl > 0) {
		st->clip_ttl--;
	}

	if (st->audio_enabled && vu != NULL) {
		st->muted = vu->muted;
		st->stereo = vu->stereo;
		if (vu->active && !vu->muted && !st->paused) {
			in_l = amp_to_norm(vu->peak_l);
			in_r = amp_to_norm(vu->stereo ? vu->peak_r : vu->peak_l);
			raw_peak = vu->peak_l > vu->peak_r ?
			    vu->peak_l : vu->peak_r;
			if (vu->clipped) {
				st->clip_ttl = 30;
			}
			if (vu->rms_l > 0.008f || vu->rms_r > 0.008f) {
				st->cur_tl_flags |= HUD_TL_AUDIO;
			}
		}
	}

	update_channel_vu(in_l, &st->vu_l, &st->hold_l, &st->hold_ttl_l);
	update_channel_vu(in_r, &st->vu_r, &st->hold_r, &st->hold_ttl_r);

	if (raw_peak > 0.001f) {
		float db = 20.0f * log10f(raw_peak);

		if (db >= st->peak_db) {
			st->peak_db = db;
		} else {
			st->peak_db -= 0.8f;
		}
	} else {
		st->peak_db -= 1.2f;
		if (st->peak_db < -60.0f) {
			st->peak_db = -60.0f;
		}
	}

	if (!st->paused) {
		step = (uint64_t)(elapsed_ms / 500.0);
		while (st->tl_last_step < step) {
			st->tl_bins[st->tl_head] = st->cur_tl_flags;
			st->tl_head = (st->tl_head + 1) % HUD_TL_BINS;
			if (st->tl_count < HUD_TL_BINS) {
				st->tl_count++;
			}
			st->cur_tl_flags = 0;
			st->tl_last_step++;
		}
	}
}

static struct tui_style
make_style(uint8_t fg_idx, uint8_t bg_idx, uint8_t bold, uint8_t reverse)
{
	struct tui_style st = tui_style_default();

	if (fg_idx != 255) {
		st.fg = tui_color_idx(fg_idx);
	}
	if (bg_idx != 255) {
		st.bg = tui_color_idx(bg_idx);
	}
	st.bold = bold;
	st.reverse = reverse;
	return st;
}

static void
format_clock(double ms, char *buf, size_t sz)
{
	long total_s = (ms >= 0.0) ? (long)(ms / 1000.0) : 0;
	long h = total_s / 3600;
	long m = (total_s % 3600) / 60;
	long s = total_s % 60;

	if (h > 0) {
		snprintf(buf, sz, "%ld:%02ld:%02ld", h, m, s);
	} else {
		snprintf(buf, sz, "%02ld:%02ld", m, s);
	}
}

static void
format_tl_cell(uint8_t flags, int live_tip, uint32_t *cp_out,
    struct tui_style *st_out)
{
	if (flags & HUD_TL_MARK) {
		*cp_out = 0x25c6; /* ◆ */
		*st_out = make_style(14, 255, 1, 0);
	} else if ((flags & (HUD_TL_TERM | HUD_TL_AUDIO)) ==
	    (HUD_TL_TERM | HUD_TL_AUDIO)) {
		*cp_out = 0x2503; /* ┃ */
		*st_out = make_style(live_tip ? 15 : 10, 255, 1, 0);
	} else if (flags & HUD_TL_TERM) {
		*cp_out = 0x2502; /* │ */
		*st_out = make_style(14, 255, 0, 0);
	} else if (flags & HUD_TL_AUDIO) {
		*cp_out = 0x254e; /* ╎ */
		*st_out = make_style(2, 255, 0, 0);
	} else {
		*cp_out = 0x00b7; /* · */
		*st_out = make_style(8, 255, 0, 0);
	}
}

static uint32_t
vu_subcell_cp(int sub)
{
	static const uint32_t blocks[9] = {
		' ', 0x2581, 0x2582, 0x2583, 0x2584,
		0x2585, 0x2586, 0x2587, 0x2588,
	};

	if (sub < 0) {
		sub = 0;
	} else if (sub > 8) {
		sub = 8;
	}
	return blocks[sub];
}

static uint8_t
vu_zone_color(float frac, int muted)
{
	if (muted) {
		return 8;
	}
	if (frac > 0.88f) {
		return 9; /* red > -7 dBFS */
	}
	if (frac > 0.68f) {
		return 11; /* yellow -19..-7 dBFS */
	}
	return 10; /* green */
}

static void
draw_vbar(struct tui_surface *sf, int top_row, int height, int col, int width,
    float level, float hold, int muted)
{
	int total_sub = height * 8;
	int filled = muted ? 0 : (int)lroundf(level * (float)total_sub);
	int hold_sub = muted ? 0 : (int)lroundf(hold * (float)total_sub);
	int hold_bot = (hold_sub > 0) ? (hold_sub - 1) / 8 : -1;

	for (int k = 0; k < height; k++) {
		int bot = height - 1 - k;
		int sub = filled - bot * 8;
		float frac = (float)(bot + 1) / (float)height;
		uint32_t cp;
		struct tui_style cst;

		if (sub > 8) {
			sub = 8;
		} else if (sub < 0) {
			sub = 0;
		}

		if (sub == 0 && !muted && bot == hold_bot && hold > 0.02f) {
			cp = 0x2594; /* ▔ floating peak cap */
			cst = make_style(vu_zone_color(frac, 0), 255, 1, 0);
		} else {
			cp = vu_subcell_cp(sub);
			cst = make_style(vu_zone_color(frac, muted), 255, 0, 0);
		}

		for (int w = 0; w < width; w++) {
			tui_surface_put_cp(sf, top_row + k, col + w, cp, &cst);
		}
	}
}

static void
render_side_hud(const struct hud_state *st, const struct hud_layout *lay,
    struct tui_surface *sf)
{
	int r0 = lay->hud_row;
	int c0 = lay->hud_col;
	int h = lay->hud_rows;
	int inner = c0 + 1;
	int iw = lay->hud_cols - 1;
	struct tui_style border_st = make_style(8, 255, 0, 0);
	struct tui_style dim_st = make_style(8, 255, 0, 0);
	struct tui_style text_st = make_style(15, 255, 0, 0);
	char buf[32];
	int foot_rows, body_top, body_bot, meter_h;

	tui_surface_fill(sf, r0, c0, h, lay->hud_cols, ' ', &text_st);
	for (int r = 0; r < h; r++) {
		tui_surface_put_cp(sf, r0 + r, c0, 0x2502 /* │ */, &border_st);
	}

	/* Row 0: Recording / Pause badge */
	if (h >= 1) {
		if (st->paused) {
			struct tui_style pst = make_style(11, 255, 1, 0);

			tui_surface_put_cp(sf, r0, inner, 0x275a /* ❚ */, &pst);
			tui_surface_put_str(sf, r0, inner + 1, iw - 1, &pst,
			    " PAUSE");
		} else {
			struct tui_style dot = make_style(9, 255, 1, 0);
			struct tui_style rec = make_style(15, 255, 1, 0);

			tui_surface_put_cp(sf, r0, inner, 0x25cf /* ● */, &dot);
			tui_surface_put_str(sf, r0, inner + 1, iw - 1, &rec,
			    st->append ? " +REC" : " REC");
		}
	}

	/* Row 1: Elapsed timeline clock */
	if (h >= 2) {
		format_clock(st->elapsed_ms, buf, sizeof buf);
		if (strlen(buf) <= 5) {
			char pad[64];

			snprintf(pad, sizeof pad, " %s", buf);
			tui_surface_put_str(sf, r0 + 1, inner, iw, &text_st,
			    pad);
		} else {
			tui_surface_put_str(sf, r0 + 1, inner, iw, &text_st,
			    buf);
		}
	}

	/* Row 2: Audio format / MUTE / NO AUD */
	if (h >= 3) {
		if (!st->audio_enabled) {
			tui_surface_put_str(sf, r0 + 2, inner, iw, &dim_st,
			    " NO AUD");
		} else if (st->muted) {
			struct tui_style mst = make_style(15, 1, 1, 0);

			tui_surface_put_str(sf, r0 + 2, inner, iw, &mst,
			    " MUTE  ");
		} else {
			char fmtbuf[16];

			struct tui_style fst = make_style(6, 255, 0, 0);

			snprintf(fmtbuf, sizeof fmtbuf, " %-6s", st->audio_fmt);
			for (int i = 1; fmtbuf[i] != '\0'; i++) {
				if (fmtbuf[i] >= 'a' && fmtbuf[i] <= 'z') {
					fmtbuf[i] -= 'a' - 'A';
				}
			}
			tui_surface_put_str(sf, r0 + 2, inner, iw, &fst,
			    fmtbuf);
		}
	}

	/* Row 3: Marker count / flash */
	if (h >= 4) {
		if (st->mark_flash_ttl > 0) {
			struct tui_style mkst = make_style(14, 255, 1, 0);

			snprintf(buf, sizeof buf, " *M:%02d",
			    st->markers > 99 ? 99 : st->markers);
			tui_surface_put_str(sf, r0 + 3, inner, iw, &mkst, buf);
		} else {
			struct tui_style mkst = make_style(st->markers > 0 ? 14 : 8,
			    255, 0, 0);

			snprintf(buf, sizeof buf, " MK:%02d",
			    st->markers > 99 ? 99 : st->markers);
			tui_surface_put_str(sf, r0 + 3, inner, iw, &mkst, buf);
		}
	}

	if (h < 7) {
		return;
	}

	tui_surface_fill(sf, r0 + 4, inner, 1, iw, 0x2500 /* ─ */, &border_st);
	tui_surface_put_cp(sf, r0 + 4, c0, 0x251c /* ├ */, &border_st);

	/* Footer rows reserved at bottom */
	foot_rows = (h >= 12) ? 3 : ((h >= 9) ? 2 : 0);
	body_top = r0 + 5;
	body_bot = r0 + h - foot_rows;

	if (body_bot > body_top + 1) {
		tui_surface_put_str(sf, body_top, inner, iw, &dim_st,
		    st->audio_enabled ?
		    (st->stereo ? "T L  R" : "T   VU") : "TIMELN");
		body_top++;
	}

	meter_h = body_bot - body_top;
	if (meter_h > 0) {
		/* Column 0 inside panel: rolling timeline activity & marker strip */
		for (int k = 0; k < meter_h; k++) {
			uint32_t cp;
			struct tui_style tst;

			if (k == 0) {
				format_tl_cell(st->cur_tl_flags, !st->paused,
				    &cp, &tst);
			} else if (k - 1 < st->tl_count) {
				int idx = (st->tl_head - k + HUD_TL_BINS) %
				    HUD_TL_BINS;

				format_tl_cell(st->tl_bins[idx], 0, &cp, &tst);
			} else {
				cp = ' ';
				tst = dim_st;
			}
			tui_surface_put_cp(sf, body_top + k, inner, cp, &tst);
		}

		/* VU meter columns */
		if (st->audio_enabled && iw >= 7) {
			if (st->stereo) {
				draw_vbar(sf, body_top, meter_h, inner + 2, 2,
				    st->vu_l, st->hold_l, st->muted);
				draw_vbar(sf, body_top, meter_h, inner + 5, 2,
				    st->vu_r, st->hold_r, st->muted);
			} else {
				draw_vbar(sf, body_top, meter_h, inner + 3, 3,
				    st->vu_l, st->hold_l, st->muted);
			}
		}
	}

	if (foot_rows >= 2) {
		int fr = r0 + h - foot_rows;

		tui_surface_fill(sf, fr, inner, 1, iw, 0x2500 /* ─ */, &border_st);
		tui_surface_put_cp(sf, fr, c0, 0x251c /* ├ */, &border_st);
		fr++;

		if (!st->audio_enabled) {
			tui_surface_put_str(sf, fr, inner, iw, &dim_st,
			    "  --  ");
		} else if (st->clip_ttl > 0) {
			struct tui_style cst = make_style(15, 1, 1, 0);

			tui_surface_put_str(sf, fr, inner, iw, &cst, " CLIP  ");
		} else if (st->muted || st->paused || st->peak_db <= -59.5f) {
			tui_surface_put_str(sf, fr, inner, iw, &dim_st,
			    " -inf ");
		} else {
			int db = (int)lroundf(st->peak_db);

			snprintf(buf, sizeof buf, " %3ddB", db);
			tui_surface_put_str(sf, fr, inner, iw, &text_st, buf);
		}
		fr++;
		if (foot_rows >= 3) {
			tui_surface_put_str(sf, fr, inner, iw, &dim_st,
			    "^apmKh");
		}
	}
}

static void
render_prompt_bar(const struct hud_state *st, int row, int col, int width,
    struct tui_surface *sf)
{
	struct tui_style bg_st = make_style(15, 0, 0, 0);
	struct tui_style pr_st = make_style(14, 0, 1, 0);
	struct tui_style dim_st = make_style(8, 0, 0, 0);
	const char *hint = " (Enter save, Esc cancel) ";
	int hint_len = (int)strlen(hint);
	int pos = col;
	char line[96];

	if (row < 0 || width <= 0) {
		return;
	}
	tui_surface_fill(sf, row, col, 1, width, ' ', &bg_st);
	tui_surface_put_cp(sf, row, pos++, ' ', &pr_st);
	if (pos < col + width) {
		tui_surface_put_cp(sf, row, pos++, 0x25c6 /* ◆ */, &pr_st);
	}
	snprintf(line, sizeof line, " Chapter: %s_", st->prompt_buf);
	int avail = width - (pos - col);
	if (avail > hint_len + 16) {
		tui_surface_put_str(sf, row, pos, avail - hint_len, &pr_st,
		    line);
		tui_surface_put_str(sf, row, col + width - hint_len, hint_len,
		    &dim_st, hint);
	} else if (avail > 0) {
		tui_surface_put_str(sf, row, pos, avail, &pr_st, line);
	}
}

static void
render_bottom_hud(const struct hud_state *st, const struct hud_layout *lay,
    struct tui_surface *sf)
{
	int r = lay->hud_row;
	int c = lay->hud_col;
	int w = lay->hud_cols;
	int pos = c;
	struct tui_style bg_st = make_style(15, 0, 0, 0);
	struct tui_style dim_st = make_style(8, 0, 0, 0);
	char buf[80];

	if (st->prompt_active) {
		render_prompt_bar(st, r, c, w, sf);
		return;
	}

	tui_surface_fill(sf, r, c, 1, w, ' ', &bg_st);

	if (st->paused) {
		struct tui_style pst = make_style(11, 0, 1, 0);

		tui_surface_put_str(sf, r, pos, w - (pos - c), &pst, "❚ PAUS ");
		pos += 7;
	} else {
		struct tui_style dot = make_style(9, 0, 1, 0);
		struct tui_style rec = make_style(15, 0, 1, 0);

		tui_surface_put_cp(sf, r, pos++, ' ', &rec);
		tui_surface_put_cp(sf, r, pos++, 0x25cf, &dot);
		tui_surface_put_str(sf, r, pos, w - (pos - c), &rec,
		    st->append ? "+REC " : " REC ");
		pos += 5;
	}

	format_clock(st->elapsed_ms, buf, sizeof buf);
	snprintf(buf + strlen(buf), sizeof buf - strlen(buf), " ");
	tui_surface_put_str(sf, r, pos, w - (pos - c), &bg_st, buf);
	pos += (int)strlen(buf);

	if (st->audio_enabled && pos + 16 < c + w) {
		if (st->muted) {
			struct tui_style mst = make_style(15, 1, 1, 0);

			tui_surface_put_str(sf, r, pos, w - (pos - c), &mst,
			    " MUTE ");
			pos += 6;
		} else {
			int bw = 8;
			float lvl = st->vu_l > st->vu_r ? st->vu_l : st->vu_r;
			int filled = (int)lroundf(lvl * (float)bw);

			tui_surface_put_str(sf, r, pos, w - (pos - c), &dim_st,
			    " VU[");
			pos += 4;
			for (int i = 0; i < bw && pos < c + w; i++, pos++) {
				float f = (float)(i + 1) / (float)bw;
				struct tui_style vst = make_style(
				    i < filled ? vu_zone_color(f, 0) : 8, 0,
				    0, 0);

				tui_surface_put_cp(sf, r, pos,
				    i < filled ? 0x2588 : 0x00b7, &vst);
			}
			tui_surface_put_str(sf, r, pos, w - (pos - c), &dim_st,
			    "] ");
			pos += 2;
		}
	}

	if (pos + 8 < c + w) {
		struct tui_style mkst = make_style(st->mark_flash_ttl > 0 ?
		    14 : 7, 0, st->mark_flash_ttl > 0, 0);

		if (st->mark_flash_ttl > 0 && st->last_marker_label[0] != '\0') {
			snprintf(buf, sizeof buf, "MK:%02d(%.16s) ",
			    st->markers, st->last_marker_label);
		} else {
			snprintf(buf, sizeof buf, "MK:%02d ", st->markers);
		}
		tui_surface_put_str(sf, r, pos, w - (pos - c), &mkst, buf);
		pos += (int)strlen(buf);
	}

	int tl_avail = (c + w) - pos - 16;
	if (tl_avail >= 6) {
		tui_surface_put_cp(sf, r, pos++, '[', &dim_st);
		for (int i = 0; i < tl_avail - 2; i++) {
			int back = (tl_avail - 3) - i;
			uint32_t cp;
			struct tui_style tst;

			if (back == 0) {
				format_tl_cell(st->cur_tl_flags, !st->paused,
				    &cp, &tst);
			} else if (back - 1 < st->tl_count) {
				int idx = (st->tl_head - back + HUD_TL_BINS) %
				    HUD_TL_BINS;

				format_tl_cell(st->tl_bins[idx], 0, &cp, &tst);
			} else {
				cp = ' ';
				tst = dim_st;
			}
			tst.bg = tui_color_idx(0);
			tui_surface_put_cp(sf, r, pos++, cp, &tst);
		}
		tui_surface_put_str(sf, r, pos, 2, &dim_st, "] ");
		pos += 2;
	}

	if (pos + 14 <= c + w) {
		tui_surface_put_str(sf, r, c + w - 14, 14, &dim_st,
		    " ^a p/m/k/K/h ");
	} else if (pos + 11 <= c + w) {
		tui_surface_put_str(sf, r, c + w - 12, 12, &dim_st,
		    " ^a p/m/K/h ");
	}
}

void
hud_render(const struct hud_state *st, const struct hud_layout *lay,
    struct tui_surface *sf)
{
	if (lay->active_mode == HUD_SIDE) {
		render_side_hud(st, lay, sf);
		if (st->prompt_active && lay->outer_rows > 0) {
			render_prompt_bar(st, lay->outer_rows - 1, 0,
			    lay->outer_cols, sf);
		}
	} else if (lay->active_mode == HUD_BOTTOM) {
		render_bottom_hud(st, lay, sf);
	} else if (st->prompt_active && lay->outer_rows > 0) {
		render_prompt_bar(st, lay->outer_rows - 1, 0,
		    lay->outer_cols, sf);
	}
}

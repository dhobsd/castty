#ifndef TUI_HUD_H
#define TUI_HUD_H

#include <stdint.h>

#include "audio.h"
#include "tui/screen.h"

enum hud_mode {
	HUD_AUTO = 0,
	HUD_SIDE,
	HUD_BOTTOM,
	HUD_OFF,
};

enum {
	HUD_SIDE_COLS = 8,
	HUD_BOTTOM_ROWS = 1,
	HUD_TL_BINS = 64,
};

enum hud_tl_flag {
	HUD_TL_TERM = 1 << 0,
	HUD_TL_AUDIO = 1 << 1,
	HUD_TL_MARK = 1 << 2,
};

struct hud_layout {
	enum hud_mode active_mode;
	int outer_rows;
	int outer_cols;
	int child_rows;
	int child_cols;
	int hud_row;
	int hud_col;
	int hud_rows;
	int hud_cols;
};

struct hud_state {
	int paused;
	int append;
	int audio_enabled;
	const char *audio_fmt;
	double elapsed_ms;
	int markers;
	double last_marker_ms;

	float vu_l;
	float vu_r;
	float hold_l;
	float hold_r;
	float peak_db;
	int hold_ttl_l;
	int hold_ttl_r;
	int clip_ttl;
	int mark_flash_ttl;
	int muted;
	int stereo;

	uint8_t tl_bins[HUD_TL_BINS];
	int tl_head;
	int tl_count;
	uint64_t tl_last_step;
	uint8_t cur_tl_flags;

	int prompt_active;
	char prompt_buf[64];
	char last_marker_label[64];
};

int hud_parse_mode(const char *arg, enum hud_mode *out);
void hud_compute_layout(enum hud_mode req_mode, int outer_rows, int outer_cols,
    int req_rows, int req_cols, int hidden, struct hud_layout *out);

void hud_init(struct hud_state *st, int paused, int append, int audio_enabled,
    const char *audio_fmt, int initial_markers, double initial_ms);
void hud_note_output(struct hud_state *st);
void hud_note_marker(struct hud_state *st, double at_ms);
void hud_note_marker_label(struct hud_state *st, double at_ms,
    const char *label);
void hud_set_prompt(struct hud_state *st, int active, const char *text);
void hud_note_pause(struct hud_state *st, int paused);
void hud_tick(struct hud_state *st, double elapsed_ms,
    const struct audio_vu *vu);
void hud_render(const struct hud_state *st, const struct hud_layout *lay,
    struct tui_surface *sf);

#endif

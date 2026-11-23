#include <sys/ioctl.h>
#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "audio/pcm_track.h"
#include "cast_doc.h"
#include "cast_player.h"
#include "castty.h"
#include "edit.h"
#include "record.h"
#include "term.h"
#include "tui/hud.h"
#include "tui/screen.h"

extern struct winsize owin, rwin, win;
extern struct termios tt;

enum {
	EDIT_UNDO_MAX = 32,
	EDIT_BOTTOM_ROWS = 2,
};

enum edit_key {
	KEY_NONE = 0,
	KEY_LEFT = 0x100,
	KEY_RIGHT,
	KEY_UP,
	KEY_DOWN,
	KEY_HOME,
	KEY_END,
	KEY_PGUP,
	KEY_PGDN,
	KEY_DEL,
	KEY_ESC = 0x1b,
	KEY_ENTER = '\r',
	KEY_BS = 0x7f,
	KEY_CTRL_R = 0x12,
	KEY_CTRL_Z = 0x1a,
};

enum edit_mode {
	MODE_PAUSE = 0,
	MODE_PLAY,
	MODE_DUB,
};

enum prompt_kind {
	PROMPT_NONE = 0,
	PROMPT_JUMP,
	PROMPT_FIND,
	PROMPT_REDACT_NEEDLE,
	PROMPT_REDACT_REPL,
	PROMPT_TITLE,
	PROMPT_MARKER_LABEL,
	PROMPT_PAUSE_SEC,
	PROMPT_IDLE_MAX_SEC,
	PROMPT_SAVE_AUDIO,
	PROMPT_QUIT_CONFIRM,
};

struct edit_undo_entry {
	struct cast_doc doc;
	struct pcm_edl_snapshot audio;
	double cur_ms;
	char desc[48];
};

struct edit_undo_stack {
	struct edit_undo_entry entries[EDIT_UNDO_MAX];
	size_t len;
	size_t pos;
};

struct edit_sel {
	int have_in;
	int have_out;
	double in_ms;
	double out_ms;
};

struct edit_dub {
	int replace;
	int frozen;
	int has_snap;
	double start_ms;
	double prev_rec_ms;
	struct edit_undo_entry snap;
};

struct edit_prompt {
	enum prompt_kind kind;
	size_t len;
	ssize_t marker_idx;
	char buf[256];
	char redact_needle[256];
	char find_query[256];
};

struct edit_ctx {
	enum edit_mode mode;
	double cur_ms;
	double speed;
	struct timespec last_wall;
	struct edit_sel sel;
	int outer_rows;
	int outer_cols;
	int pan_row;
	int pan_col;
	int hud_override;
	int ungot_byte;
	int show_help;
	int running;

	struct cast_player *player;
	struct pcm_audio_engine *eng;
	struct tui_surface *sf;
	struct hud_state hud;
	struct cast_doc doc;
	struct pcm_track track;

	const char *cast_path;
	char *audio_path;
	enum audio_format audio_fmt;
	const char *in_devid;
	const char *out_devid;
	int use_raw;

	struct edit_dub dub;
	struct edit_prompt prompt;
	char status[256];
	struct edit_undo_stack undo;
};

static volatile sig_atomic_t edit_winched;

static void
on_edit_sigwinch(int sig)
{
	(void)sig;
	edit_winched = 1;
}

static double
wall_elapsed_ms(struct timespec *prev)
{
	struct timespec now;
	double ms;

	clock_gettime(CLOCK_MONOTONIC, &now);
	ms = (double)(now.tv_sec - prev->tv_sec) * 1000.0 +
	    (double)(now.tv_nsec - prev->tv_nsec) / 1000000.0;
	*prev = now;
	return (ms > 0.0) ? ms : 0.0;
}

static void
edit_usage(int status)
{
	fprintf(stderr,
	    "usage: castty edit [-a <audio>] [-d <in_dev>] [-F <format>] [-l] [-o <out_dev>] [-R] <file.cast>\n"
	    " -a <audio>     Accompanying audio file to edit/save alongside the cast.\n"
	    " -d <in_dev>    Audio input device ID for voiceover/punch-in dubbing.\n"
	    " -F <format>    Audio format: raw, mp3, wav, flac, opus, ogg, m4a.\n"
	    " -h             Show this help.\n"
	    " -l             List available audio input and output devices and exit.\n"
	    " -o <out_dev>   Audio output device ID for playback.\n"
	    " -R             Use raw audio devices.\n");
	exit(status);
}

static void
undo_entry_free(struct edit_undo_entry *e)
{
	cast_doc_free(&e->doc);
	pcm_edl_snapshot_free(&e->audio);
}

static int
undo_capture(struct edit_ctx *ed, struct edit_undo_entry *out, const char *desc)
{
	memset(out, 0, sizeof *out);
	if (cast_doc_clone(&ed->doc, &out->doc) != 0) {
		return -1;
	}
	if (pcm_edl_snapshot_take(&ed->track, &out->audio) != 0) {
		cast_doc_free(&out->doc);
		return -1;
	}
	out->cur_ms = ed->cur_ms;
	snprintf(out->desc, sizeof out->desc, "%s", desc ? desc : "edit");
	return 0;
}

static void
open_prompt(struct edit_ctx *ed, enum prompt_kind kind, const char *init)
{
	ed->prompt.kind = kind;
	if (init != NULL && *init != '\0') {
		snprintf(ed->prompt.buf, sizeof ed->prompt.buf, "%s", init);
		ed->prompt.len = strlen(ed->prompt.buf);
	} else {
		ed->prompt.buf[0] = '\0';
		ed->prompt.len = 0;
	}
}

static void
close_prompt(struct edit_ctx *ed)
{
	ed->prompt.kind = PROMPT_NONE;
	ed->prompt.buf[0] = '\0';
	ed->prompt.len = 0;
}

static void
undo_push_entry(struct edit_ctx *ed, struct edit_undo_entry *entry)
{
	struct edit_undo_stack *u = &ed->undo;

	for (size_t i = u->pos; i < u->len; i++) {
		undo_entry_free(&u->entries[i]);
	}
	u->len = u->pos;
	if (u->len == EDIT_UNDO_MAX) {
		undo_entry_free(&u->entries[0]);
		memmove(&u->entries[0], &u->entries[1],
		    (EDIT_UNDO_MAX - 1) * sizeof u->entries[0]);
		u->len--;
		u->pos--;
	}
	u->entries[u->len++] = *entry;
	u->pos = u->len;
	memset(entry, 0, sizeof *entry);
}

static int
undo_save_current(struct edit_ctx *ed, const char *desc)
{
	struct edit_undo_entry e;

	if (undo_capture(ed, &e, desc) != 0) {
		return -1;
	}
	undo_push_entry(ed, &e);
	return 0;
}

static void
sync_hud_markers(struct edit_ctx *ed)
{
	int m = 0;

	for (size_t i = 0; i < ed->doc.nevents; i++) {
		if (ed->doc.events[i].type == CAST_EV_MARKER) {
			m++;
		}
	}
	ed->hud.markers = m;
}

static void
clamp_cur_ms(struct edit_ctx *ed)
{
	double dur = ed->doc.duration_ms;

	if (ed->track.has_audio && pcm_track_duration_ms(&ed->track) > dur) {
		dur = pcm_track_duration_ms(&ed->track);
	}
	if (ed->cur_ms < 0.0) {
		ed->cur_ms = 0.0;
	}
	if (ed->cur_ms > dur) {
		ed->cur_ms = dur;
	}
}

static double
total_timeline_ms(const struct edit_ctx *ed)
{
	double d = ed->doc.duration_ms;
	double ad = ed->track.has_audio ?
	    pcm_track_duration_ms(&ed->track) : 0.0;

	return (ad > d) ? ad : d;
}

static void
stop_playback(struct edit_ctx *ed)
{
	if (ed->mode == MODE_PLAY) {
		if (ed->track.has_audio && pcm_engine_is_playing(ed->eng)) {
			ed->cur_ms = pcm_engine_play_pos_ms(ed->eng);
		}
		pcm_engine_stop_play(ed->eng);
		ed->mode = MODE_PAUSE;
		hud_note_pause(&ed->hud, 1);
	}
}

static int
side_hud_visible(const struct edit_ctx *ed)
{
	if (ed->outer_cols < 48) {
		return 0;
	}
	if (ed->hud_override != 0) {
		return ed->hud_override > 0;
	}
	return ed->mode == MODE_DUB ||
	    ed->outer_cols >= cast_player_cols(ed->player) + HUD_SIDE_COLS;
}

static void
viewport_dims(const struct edit_ctx *ed, int *vp_rows_out, int *vp_cols_out)
{
	int side_cols = side_hud_visible(ed) ? HUD_SIDE_COLS : 0;
	int vr = ed->outer_rows - EDIT_BOTTOM_ROWS;
	int vc = ed->outer_cols - side_cols;

	*vp_rows_out = vr > 1 ? vr : 1;
	*vp_cols_out = vc > 1 ? vc : 1;
}

static void
clamp_pan(struct edit_ctx *ed, int vp_rows, int vp_cols)
{
	int max_r = cast_player_rows(ed->player) - vp_rows;
	int max_c = cast_player_cols(ed->player) - vp_cols;

	if (max_r < 0) {
		max_r = 0;
	}
	if (max_c < 0) {
		max_c = 0;
	}
	if (ed->pan_row < 0) {
		ed->pan_row = 0;
	} else if (ed->pan_row > max_r) {
		ed->pan_row = max_r;
	}
	if (ed->pan_col < 0) {
		ed->pan_col = 0;
	} else if (ed->pan_col > max_c) {
		ed->pan_col = max_c;
	}
}

static void
follow_cursor_pan(struct edit_ctx *ed)
{
	int vp_rows = 1, vp_cols = 1;
	int cr = 0, cc = 0;

	if (ed->player == NULL) {
		return;
	}
	viewport_dims(ed, &vp_rows, &vp_cols);
	cast_player_get_cursor(ed->player, &cr, &cc, NULL, NULL);
	if (cr < ed->pan_row) {
		ed->pan_row = cr;
	} else if (cr >= ed->pan_row + vp_rows) {
		ed->pan_row = cr - vp_rows + 1;
	}
	if (cc < ed->pan_col) {
		ed->pan_col = cc;
	} else if (cc >= ed->pan_col + vp_cols) {
		ed->pan_col = cc - vp_cols + 1;
	}
	clamp_pan(ed, vp_rows, vp_cols);
}

static void
seek_to(struct edit_ctx *ed, double ms)
{
	double tot = total_timeline_ms(ed);

	if (ms < 0.0) {
		ms = 0.0;
	}
	if (ms > tot) {
		ms = tot;
	}
	ed->cur_ms = ms;
	cast_player_seek(ed->player, ed->cur_ms);
	follow_cursor_pan(ed);
}

static int
get_selection(const struct edit_ctx *ed, double *t0_out, double *t1_out)
{
	double a, b;

	if (!ed->sel.have_in || !ed->sel.have_out) {
		return 0;
	}
	a = ed->sel.in_ms < ed->sel.out_ms ? ed->sel.in_ms : ed->sel.out_ms;
	b = ed->sel.in_ms < ed->sel.out_ms ? ed->sel.out_ms : ed->sel.in_ms;
	if (b - a < 10.0) {
		return 0;
	}
	*t0_out = a;
	*t1_out = b;
	return 1;
}

static void
set_status(struct edit_ctx *ed, const char *msg)
{
	snprintf(ed->status, sizeof ed->status, "%s", msg ? msg : "");
}

static void
perform_undo(struct edit_ctx *ed, int is_redo)
{
	struct edit_undo_stack *u = &ed->undo;
	struct edit_undo_entry tmp;

	stop_playback(ed);
	if (!is_redo) {
		if (u->pos == 0) {
			set_status(ed, "Nothing to undo");
			return;
		}
		if (undo_capture(ed, &tmp, "redo-cur") != 0) {
			return;
		}
		u->pos--;
		struct edit_undo_entry target = u->entries[u->pos];
		u->entries[u->pos] = tmp;

		cast_doc_free(&ed->doc);
		ed->doc = target.doc;
		(void)pcm_edl_snapshot_restore(&ed->track, &target.audio);
		pcm_edl_snapshot_free(&target.audio);
		cast_player_invalidate(ed->player);
		sync_hud_markers(ed);
		seek_to(ed, target.cur_ms);
		ed->sel.have_in = ed->sel.have_out = 0;
		snprintf(ed->status, sizeof ed->status, "Undid: %s",
		    target.desc);
	} else {
		if (u->pos >= u->len) {
			set_status(ed, "Nothing to redo");
			return;
		}
		if (undo_capture(ed, &tmp, "undo-cur") != 0) {
			return;
		}
		struct edit_undo_entry target = u->entries[u->pos];
		u->entries[u->pos] = tmp;
		u->pos++;

		cast_doc_free(&ed->doc);
		ed->doc = target.doc;
		(void)pcm_edl_snapshot_restore(&ed->track, &target.audio);
		pcm_edl_snapshot_free(&target.audio);
		cast_player_invalidate(ed->player);
		sync_hud_markers(ed);
		seek_to(ed, target.cur_ms);
		ed->sel.have_in = ed->sel.have_out = 0;
		set_status(ed, "Redid edit");
	}
}

static void
do_save(struct edit_ctx *ed, const char *save_audio_path)
{
	char err[256];

	stop_playback(ed);
	if (save_audio_path != NULL && *save_audio_path != '\0') {
		free(ed->audio_path);
		ed->audio_path = strdup(save_audio_path);
	}

	if (cast_doc_save(&ed->doc, ed->cast_path, err, sizeof err) != 0) {
		set_status(ed, err);
		return;
	}
	if (ed->track.has_audio) {
		if (ed->audio_path == NULL || *ed->audio_path == '\0') {
			open_prompt(ed, PROMPT_SAVE_AUDIO, NULL);
			set_status(ed,
			    "Cast saved; enter audio path to save track");
			return;
		}
		if (pcm_track_save(&ed->track, ed->audio_path, ed->audio_fmt,
		    !ed->track.source_stereo, err, sizeof err) != 0) {
			set_status(ed, err);
			return;
		}
	}
	ed->doc.dirty = 0;
	snprintf(ed->status, sizeof ed->status, "Saved %s%s%s",
	    ed->cast_path,
	    (ed->track.has_audio && ed->audio_path) ? " + " : "",
	    (ed->track.has_audio && ed->audio_path) ? ed->audio_path : "");
}

static void
start_playback(struct edit_ctx *ed)
{
	char err[256];

	if (ed->mode == MODE_DUB) {
		return;
	}
	if (ed->cur_ms >= total_timeline_ms(ed) - 10.0) {
		seek_to(ed, 0.0);
	}
	if (ed->track.has_audio) {
		if (pcm_engine_start_play(ed->eng, ed->cur_ms, ed->speed, err,
		    sizeof err) != 0) {
			set_status(ed, err);
			return;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &ed->last_wall);
	ed->mode = MODE_PLAY;
	hud_note_pause(&ed->hud, 0);
	set_status(ed, "Playing (Space to pause)");
}

static void
start_dub(struct edit_ctx *ed, int replace_mode)
{
	struct edit_dub *d = &ed->dub;
	char err[256];

	stop_playback(ed);
	pcm_track_ensure_peaks(&ed->track);
	memset(d, 0, sizeof *d);
	if (undo_capture(ed, &d->snap,
	    replace_mode ? "voiceover" : "overdub") != 0) {
		set_status(ed, "Out of memory starting dub");
		return;
	}
	d->has_snap = 1;

	if (pcm_engine_start_dub(ed->eng, err, sizeof err) != 0) {
		undo_entry_free(&d->snap);
		d->has_snap = 0;
		set_status(ed, err);
		return;
	}
	d->replace = replace_mode;
	d->start_ms = ed->cur_ms;
	ed->mode = MODE_DUB;
	clock_gettime(CLOCK_MONOTONIC, &ed->last_wall);
	hud_note_pause(&ed->hud, 0);
	set_status(ed, replace_mode ?
	    "DUB [Replace]: Space=Freeze/Extend .=Step Enter=Commit Esc=Cancel" :
	    "DUB [Mix]: Space=Freeze/Extend .=Step Enter=Commit Esc=Cancel");
}

static void
finish_dub(struct edit_ctx *ed, int commit)
{
	struct edit_dub *d = &ed->dub;
	int buf_id = -1;
	uint64_t nf = 0;

	if (ed->mode != MODE_DUB) {
		return;
	}
	(void)pcm_engine_stop_dub(ed->eng, &buf_id, &nf);
	ed->mode = MODE_PAUSE;
	hud_note_pause(&ed->hud, 1);

	if (!commit || nf == 0) {
		if (d->has_snap) {
			cast_doc_free(&ed->doc);
			ed->doc = d->snap.doc;
			(void)pcm_edl_snapshot_restore(&ed->track,
			    &d->snap.audio);
			pcm_edl_snapshot_free(&d->snap.audio);
			d->has_snap = 0;
			cast_player_invalidate(ed->player);
			sync_hud_markers(ed);
			seek_to(ed, d->snap.cur_ms);
		}
		set_status(ed, commit ? "No audio captured" : "Dub cancelled");
		return;
	}

	if (d->has_snap) {
		undo_push_entry(ed, &d->snap);
		d->has_snap = 0;
	}
	if (pcm_track_apply_recording(&ed->track, d->start_ms, buf_id, 0, nf,
	    d->replace, 0.25f) != 0) {
		set_status(ed, "Failed applying dub track");
		return;
	}
	if (pcm_track_duration_ms(&ed->track) > ed->doc.duration_ms) {
		ed->doc.duration_ms = pcm_track_duration_ms(&ed->track);
	}
	ed->doc.dirty = 1;
	ed->hud.audio_enabled = 1;
	if (ed->audio_path == NULL) {
		ed->hud.audio_fmt = "m4a";
	}
	cast_player_invalidate(ed->player);
	seek_to(ed, ed->cur_ms);
	snprintf(ed->status, sizeof ed->status,
	    "Committed %.2fs %s dub at %.2fs",
	    (double)nf / (double)PCM_TRACK_RATE,
	    d->replace ? "replace" : "mix",
	    d->start_ms / 1000.0);
}

static void
dub_step_next_event(struct edit_ctx *ed)
{
	double next_t = -1.0;

	for (size_t i = 0; i < ed->doc.nevents; i++) {
		if ((ed->doc.events[i].type == CAST_EV_OUT ||
		     ed->doc.events[i].type == CAST_EV_RESIZE) &&
		    ed->doc.events[i].t_ms > ed->cur_ms + 0.5) {
			next_t = ed->doc.events[i].t_ms;
			break;
		}
	}
	if (next_t < 0.0) {
		set_status(ed, "No further output events to pull");
		return;
	}

	/* Cut dead span before next_t so events remain ordered at cur_ms. */
	if (next_t > ed->cur_ms + 0.01) {
		(void)cast_doc_cut_range(&ed->doc, ed->cur_ms, next_t - 0.001,
		    NULL, 0, 0, 0);
		(void)pcm_track_cut_range(&ed->track, ed->cur_ms,
		    next_t - 0.001);
	}
	ed->cur_ms += 0.002;
	cast_player_invalidate(ed->player);
	cast_player_seek(ed->player, ed->cur_ms);
	follow_cursor_pan(ed);
	set_status(ed, "Stepped next event to playhead");
}

static void
do_cut_selection(struct edit_ctx *ed)
{
	double t0, t1;
	char *snap = NULL;
	size_t slen = 0;
	int scols = 0, srows = 0, had_vis = 0;

	stop_playback(ed);
	if (!get_selection(ed, &t0, &t1)) {
		set_status(ed, "Mark In (i) and Out (o) before cutting");
		return;
	}
	if (undo_save_current(ed, "cut range") != 0) {
		return;
	}

	for (size_t i = 0; i < ed->doc.nevents; i++) {
		if (ed->doc.events[i].t_ms > t0 &&
		    ed->doc.events[i].t_ms <= t1 &&
		    (ed->doc.events[i].type == CAST_EV_OUT ||
		     ed->doc.events[i].type == CAST_EV_RESIZE)) {
			had_vis = 1;
			break;
		}
	}
	if (had_vis) {
		(void)cast_player_snapshot_ansi(&ed->doc, t1, &snap, &slen,
		    &scols, &srows);
	}
	if (cast_doc_cut_range(&ed->doc, t0, t1, snap, slen, scols,
	    srows) != 0) {
		free(snap);
		set_status(ed, "Cut failed");
		return;
	}
	free(snap);

	(void)pcm_track_cut_range(&ed->track, t0, t1);
	cast_player_invalidate(ed->player);
	sync_hud_markers(ed);
	ed->sel.have_in = ed->sel.have_out = 0;
	seek_to(ed, t0);
	snprintf(ed->status, sizeof ed->status, "Cut %.2fs [%.2f..%.2fs]",
	    (t1 - t0) / 1000.0, t0 / 1000.0, t1 / 1000.0);
}

static void
do_mute_selection(struct edit_ctx *ed)
{
	double t0, t1;

	stop_playback(ed);
	if (!ed->track.has_audio) {
		set_status(ed, "No audio track loaded");
		return;
	}
	if (!get_selection(ed, &t0, &t1)) {
		set_status(ed, "Mark In (i) and Out (o) to mute range");
		return;
	}
	if (undo_save_current(ed, "mute range") != 0) {
		return;
	}
	(void)pcm_track_mute_range(&ed->track, t0, t1);
	ed->doc.dirty = 1;
	ed->sel.have_in = ed->sel.have_out = 0;
	snprintf(ed->status, sizeof ed->status, "Muted %.2f..%.2fs",
	    t0 / 1000.0, t1 / 1000.0);
}

static void
do_compress_idle(struct edit_ctx *ed, double max_gap_s)
{
	double t0 = 0.0, t1 = ed->doc.duration_ms;
	struct cast_cut_gap *gaps = NULL;
	size_t ngaps = 0;
	double saved = 0.0;

	stop_playback(ed);
	(void)get_selection(ed, &t0, &t1);
	if (undo_save_current(ed, "compress idle") != 0) {
		return;
	}
	if (cast_doc_compress_idle(&ed->doc, t0, t1, max_gap_s * 1000.0, &gaps,
	    &ngaps, &saved) != 0) {
		set_status(ed, "Compress failed");
		return;
	}
	(void)pcm_track_cut_gaps(&ed->track, gaps, ngaps);
	free(gaps);
	cast_player_invalidate(ed->player);
	seek_to(ed, ed->cur_ms <= t0 ? ed->cur_ms : t0);
	snprintf(ed->status, sizeof ed->status,
	    "Compressed %zu idle gaps (saved %.2fs)", ngaps, saved / 1000.0);
}

static void
do_insert_pause(struct edit_ctx *ed, double add_s)
{
	if (add_s <= 0.0) {
		return;
	}
	stop_playback(ed);
	if (undo_save_current(ed, "insert pause") != 0) {
		return;
	}
	(void)cast_doc_insert_pause(&ed->doc, ed->cur_ms, add_s * 1000.0);
	(void)pcm_track_insert_silence(&ed->track, ed->cur_ms, add_s * 1000.0);
	cast_player_invalidate(ed->player);
	seek_to(ed, ed->cur_ms);
	snprintf(ed->status, sizeof ed->status, "Inserted %.2fs pause at %.2fs",
	    add_s, ed->cur_ms / 1000.0);
}

static void
do_nudge_audio(struct edit_ctx *ed, double delta_ms)
{
	double min_dur = 0.0, adur;

	stop_playback(ed);
	if (!ed->track.has_audio) {
		set_status(ed, "No audio track to nudge");
		return;
	}
	if (undo_save_current(ed, "nudge audio") != 0) {
		return;
	}
	(void)pcm_track_nudge(&ed->track, delta_ms);
	if (ed->doc.nevents > 0) {
		min_dur = ed->doc.events[ed->doc.nevents - 1].t_ms;
	}
	adur = pcm_track_duration_ms(&ed->track);
	ed->doc.duration_ms = (adur > min_dur) ? adur : min_dur;
	ed->doc.dirty = 1;
	snprintf(ed->status, sizeof ed->status,
	    "Nudged audio %+.0fms (dur %.2fs)", delta_ms, adur / 1000.0);
}

static ssize_t
find_marker_near(const struct cast_doc *doc, double t_ms, double tol_ms)
{
	ssize_t best = -1;
	double best_d = tol_ms + 1.0;

	for (size_t i = 0; i < doc->nevents; i++) {
		if (doc->events[i].type == CAST_EV_MARKER) {
			double d = fabs(doc->events[i].t_ms - t_ms);

			if (d <= tol_ms && d < best_d) {
				best = (ssize_t)i;
				best_d = d;
			}
		}
	}
	return best;
}

static void
step_term_event(struct edit_ctx *ed, int dir)
{
	stop_playback(ed);
	if (dir > 0) {
		for (size_t i = 0; i < ed->doc.nevents; i++) {
			if ((ed->doc.events[i].type == CAST_EV_OUT ||
			     ed->doc.events[i].type == CAST_EV_RESIZE) &&
			    ed->doc.events[i].t_ms > ed->cur_ms + 0.5) {
				seek_to(ed, ed->doc.events[i].t_ms);
				return;
			}
		}
		seek_to(ed, total_timeline_ms(ed));
	} else {
		for (size_t i = ed->doc.nevents; i > 0; i--) {
			size_t idx = i - 1;

			if ((ed->doc.events[idx].type == CAST_EV_OUT ||
			     ed->doc.events[idx].type == CAST_EV_RESIZE) &&
			    ed->doc.events[idx].t_ms < ed->cur_ms - 0.5) {
				seek_to(ed, ed->doc.events[idx].t_ms);
				return;
			}
		}
		seek_to(ed, 0.0);
	}
}

static int
parse_time_str(const char *s, double *ms_out)
{
	double a = 0.0, b = 0.0, c = 0.0;
	int n = 0;

	if (strchr(s, ':') != NULL) {
		n = sscanf(s, "%lf:%lf:%lf", &a, &b, &c);
		if (n == 2) {
			*ms_out = (a * 60.0 + b) * 1000.0;
			return 0;
		}
		if (n == 3) {
			*ms_out = (a * 3600.0 + b * 60.0 + c) * 1000.0;
			return 0;
		}
		return -1;
	}
	if (sscanf(s, "%lf", &a) == 1 && a >= 0.0) {
		*ms_out = a * 1000.0;
		return 0;
	}
	return -1;
}

static void
submit_prompt(struct edit_ctx *ed)
{
	struct edit_prompt *pr = &ed->prompt;
	enum prompt_kind kind = pr->kind;
	char buf[256];

	snprintf(buf, sizeof buf, "%s", pr->buf);
	close_prompt(ed);

	switch (kind) {
	case PROMPT_JUMP: {
		double ms = 0.0;

		if (parse_time_str(buf, &ms) == 0) {
			stop_playback(ed);
			seek_to(ed, ms);
			snprintf(ed->status, sizeof ed->status, "Jumped to %.2fs",
			    ed->cur_ms / 1000.0);
		} else {
			set_status(ed, "Invalid timestamp (use SS.S or MM:SS)");
		}
		break;
	}
	case PROMPT_FIND: {
		size_t idx = 0;

		if (*buf != '\0') {
			snprintf(pr->find_query, sizeof pr->find_query, "%s",
			    buf);
		}
		if (*pr->find_query == '\0') {
			break;
		}
		if (cast_doc_find_text(&ed->doc, pr->find_query, ed->cur_ms, 1,
		    &idx) == 0) {
			stop_playback(ed);
			seek_to(ed, ed->doc.events[idx].t_ms);
			snprintf(ed->status, sizeof ed->status,
			    "Found \"%.64s\" at %.2fs", pr->find_query,
			    ed->cur_ms / 1000.0);
		} else {
			snprintf(ed->status, sizeof ed->status,
			    "Not found ahead: \"%.64s\"", pr->find_query);
		}
		break;
	}
	case PROMPT_REDACT_NEEDLE:
		if (*buf == '\0') {
			set_status(ed, "Redact cancelled");
			break;
		}
		snprintf(pr->redact_needle, sizeof pr->redact_needle, "%s", buf);
		open_prompt(ed, PROMPT_REDACT_REPL, "****");
		break;
	case PROMPT_REDACT_REPL:
		if (undo_save_current(ed, "redact") == 0) {
			size_t n = cast_doc_redact(&ed->doc, pr->redact_needle,
			    buf);

			cast_player_invalidate(ed->player);
			seek_to(ed, ed->cur_ms);
			snprintf(ed->status, sizeof ed->status,
			    "Redacted %zu occurrences", n);
		}
		break;
	case PROMPT_TITLE:
		if (undo_save_current(ed, "title") == 0) {
			cast_doc_set_title(&ed->doc, buf);
			snprintf(ed->status, sizeof ed->status,
			    "Updated title: %.64s",
			    ed->doc.title ? ed->doc.title : "(cleared)");
		}
		break;
	case PROMPT_MARKER_LABEL:
		if (undo_save_current(ed, "marker") == 0) {
			if (pr->marker_idx >= 0 &&
			    (size_t)pr->marker_idx < ed->doc.nevents &&
			    ed->doc.events[pr->marker_idx].type ==
			    CAST_EV_MARKER) {
				(void)cast_doc_set_marker_label(&ed->doc,
				    (size_t)pr->marker_idx, buf);
				snprintf(ed->status, sizeof ed->status,
				    "Updated marker \"%.64s\" at %.2fs", buf,
				    ed->doc.events[pr->marker_idx].t_ms /
				    1000.0);
			} else {
				(void)cast_doc_add_marker(&ed->doc, ed->cur_ms,
				    buf);
				hud_note_marker(&ed->hud, ed->cur_ms);
				snprintf(ed->status, sizeof ed->status,
				    "Added marker \"%.64s\" at %.2fs", buf,
				    ed->cur_ms / 1000.0);
			}
			sync_hud_markers(ed);
		}
		pr->marker_idx = -1;
		break;
	case PROMPT_PAUSE_SEC: {
		double s = (*buf != '\0') ? strtod(buf, NULL) : 1.0;

		if (s > 0.0) {
			do_insert_pause(ed, s);
		}
		break;
	}
	case PROMPT_IDLE_MAX_SEC: {
		double s = (*buf != '\0') ? strtod(buf, NULL) : 1.0;

		if (s > 0.0) {
			do_compress_idle(ed, s);
		}
		break;
	}
	case PROMPT_SAVE_AUDIO:
		if (*buf != '\0') {
			do_save(ed, buf);
		}
		break;
	case PROMPT_QUIT_CONFIRM:
		if (buf[0] == 'y' || buf[0] == 'Y') {
			ed->running = 0;
		}
		break;
	case PROMPT_NONE:
		break;
	}
}

static int
read_byte_timed(struct edit_ctx *ed, int fd, int timeout_ms, unsigned char *out)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	if (ed->ungot_byte >= 0) {
		*out = (unsigned char)ed->ungot_byte;
		ed->ungot_byte = -1;
		return 1;
	}
	if (poll(&pfd, 1, timeout_ms) <= 0 || !(pfd.revents & POLLIN)) {
		return 0;
	}
	return read(fd, out, 1) == 1;
}

static int
read_key(struct edit_ctx *ed, int fd, int timeout_ms)
{
	unsigned char ch, c1;

	if (!read_byte_timed(ed, fd, timeout_ms, &ch)) {
		return KEY_NONE;
	}
	if (ch != 0x1b) {
		return (int)ch;
	}
	if (!read_byte_timed(ed, fd, 25, &c1)) {
		return KEY_ESC;
	}
	if (c1 != '[' && c1 != 'O') {
		ed->ungot_byte = (int)c1;
		return KEY_ESC;
	}

	if (c1 == 'O') {
		unsigned char c2;

		if (!read_byte_timed(ed, fd, 25, &c2)) {
			return KEY_ESC;
		}
		switch (c2) {
		case 'A':
			return KEY_UP;
		case 'B':
			return KEY_DOWN;
		case 'C':
			return KEY_RIGHT;
		case 'D':
			return KEY_LEFT;
		case 'H':
			return KEY_HOME;
		case 'F':
			return KEY_END;
		default:
			return KEY_ESC;
		}
	}

	unsigned char param = 0, fin = 0;
	for (int i = 0; i < 8; i++) {
		unsigned char b;

		if (!read_byte_timed(ed, fd, 25, &b)) {
			return KEY_ESC;
		}
		if (b >= 0x40 && b <= 0x7e) {
			fin = b;
			break;
		}
		if (param == 0 && b >= '0' && b <= '9') {
			param = b;
		}
	}

	switch (fin) {
	case 'A':
		return KEY_UP;
	case 'B':
		return KEY_DOWN;
	case 'C':
		return KEY_RIGHT;
	case 'D':
		return KEY_LEFT;
	case 'H':
		return KEY_HOME;
	case 'F':
		return KEY_END;
	case '~':
		switch (param) {
		case '1':
		case '7':
			return KEY_HOME;
		case '3':
			return KEY_DEL;
		case '4':
		case '8':
			return KEY_END;
		case '5':
			return KEY_PGUP;
		case '6':
			return KEY_PGDN;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return KEY_ESC;
}

static void
render_help_overlay(struct tui_surface *sf, int vp_rows, int vp_cols)
{
	static const char *lines[] = {
		" castty edit - Keyboard Reference (? / Esc to close) ",
		" Transport:  Space Play/Pause   h/l,Left/Right -/+0.5s ",
		"             [/] -/+5s   ,/. Prev/Next Ev   0/$ Bounds ",
		"             n/p Next/Prev Marker   +/- Speed  : Jump  ",
		"             Up/Down Pan viewport   Tab Toggle HUD     ",
		" Selection:  i Set In   o Set Out   c Clear Selection  ",
		" Editing:    x Cut [In..Out] (crossfades + VTerm snap) ",
		"             m Mute audio [In..Out]   s Compress idle  ",
		"             P Insert pause/hold   (/) Audio nudge 50ms",
		"             R Redact text   / Find text   T Edit title",
		" Markers:    k Quick marker   K Add/rename   d Delete  ",
		" Voiceover:  V Dub (Replace)   O Overdub (Mix ducked)  ",
		"             During Dub: Space Freeze/Extend  . Step   ",
		"                         Enter Commit   Esc Cancel     ",
		" Undo/Save:  u / Ctrl-Z Undo   U / Ctrl-R Redo         ",
		"             w Save atomically   q Quit (Q Force quit) ",
	};
	int nlines = (int)(sizeof lines / sizeof lines[0]);
	int w = 56;
	int h = nlines + 2;

	if (w > vp_cols) {
		w = vp_cols;
	}
	if (h > vp_rows) {
		h = vp_rows;
	}
	int r0 = (vp_rows - h) / 2;
	int c0 = (vp_cols - w) / 2;
	VTermColor bg = tui_color_idx(0), fg = tui_color_idx(15);
	VTermColor hd = tui_color_idx(14);
	struct tui_style box_st = { .fg = fg, .bg = bg };
	struct tui_style hdr_st = { .fg = hd, .bg = bg, .bold = 1 };

	tui_surface_fill(sf, r0, c0, h, w, ' ', &box_st);
	for (int i = 0; i < nlines && i + 1 < h; i++) {
		tui_surface_put_str(sf, r0 + 1 + i, c0 + 1, w - 2,
		    i == 0 ? &hdr_st : &box_st, lines[i]);
	}
}

static void
render_editor(struct edit_ctx *ed)
{
	int has_side = side_hud_visible(ed);
	int vp_rows = 1, vp_cols = 1;

	viewport_dims(ed, &vp_rows, &vp_cols);
	clamp_pan(ed, vp_rows, vp_cols);

	tui_surface_begin(ed->sf);
	cast_player_blit(ed->player, ed->sf, ed->pan_row, ed->pan_col, 0, 0,
	    vp_rows, vp_cols);

	/* Side HUD (VU meter + clock + markers + live state) */
	if (has_side) {
		struct hud_layout lay = {
			.active_mode = HUD_SIDE,
			.outer_rows = ed->outer_rows,
			.outer_cols = ed->outer_cols,
			.child_rows = vp_rows,
			.child_cols = vp_cols,
			.hud_row = 0,
			.hud_col = vp_cols,
			.hud_rows = vp_rows,
			.hud_cols = HUD_SIDE_COLS,
		};

		ed->hud.muted = (ed->mode == MODE_PLAY &&
		    pcm_engine_get_play_mute(ed->eng));
		hud_render(&ed->hud, &lay, ed->sf);
	}

	/* Timeline + waveform scrubber bar (row vp_rows) */
	int tl_row = vp_rows;
	double tot_ms = total_timeline_ms(ed);
	double sel0 = 0.0, sel1 = 0.0;
	int has_sel = get_selection(ed, &sel0, &sel1);
	char time_rhs[64];
	int cur_s = (int)(ed->cur_ms / 1000.0);
	int tot_s = (int)(tot_ms / 1000.0);
	const char *mode_tag = (ed->mode == MODE_DUB) ?
	    (ed->dub.frozen ? "[DUB+]" : "[DUB]") :
	    ((ed->mode == MODE_PLAY) ? "[PLAY]" : "[PAUS]");

	snprintf(time_rhs, sizeof time_rhs, " %s %02d:%02d.%d/%02d:%02d x%.1f%s",
	    mode_tag, cur_s / 60, cur_s % 60,
	    ((int)(ed->cur_ms / 100.0)) % 10, tot_s / 60, tot_s % 60,
	    ed->speed, ed->doc.dirty ? "*" : "");
	int rhs_len = (int)strlen(time_rhs);
	int bar_w = ed->outer_cols - rhs_len - 2;

	if (bar_w < 8) {
		bar_w = ed->outer_cols - 2;
		rhs_len = 0;
	}

	struct tui_style bar_bg = {
		.fg = tui_color_idx(7),
		.bg = tui_color_idx(0),
	};
	tui_surface_fill(ed->sf, tl_row, 0, 1, ed->outer_cols, ' ', &bar_bg);

	static const uint32_t wave_glyphs[8] = {
		0x2500, 0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587,
	};

	if (bar_w > 0) {
		int head_col = (tot_ms > 0.0) ?
		    (int)llround((ed->cur_ms / tot_ms) * (double)(bar_w - 1)) :
		    0;

		if (ed->mode != MODE_DUB && ed->track.has_audio) {
			pcm_track_ensure_peaks(&ed->track);
		}
		if (head_col < 0) {
			head_col = 0;
		} else if (head_col >= bar_w) {
			head_col = bar_w - 1;
		}
		tui_surface_put_cp(ed->sf, tl_row, 0, '[', &bar_bg);
		for (int i = 0; i < bar_w; i++) {
			double b0 = (tot_ms > 0.0) ?
			    ((double)i / (double)bar_w) * tot_ms : 0.0;
			double b1 = (tot_ms > 0.0) ?
			    ((double)(i + 1) / (double)bar_w) * tot_ms : 0.0;
			int in_sel = (has_sel && b1 >= sel0 && b0 <= sel1);
			int is_mark = 0;

			for (size_t k = 0; k < ed->doc.nevents; k++) {
				if (ed->doc.events[k].type == CAST_EV_MARKER &&
				    ed->doc.events[k].t_ms >= b0 &&
				    ed->doc.events[k].t_ms <= b1 + 1.0) {
					is_mark = 1;
					break;
				}
			}

			struct tui_style cst = bar_bg;
			uint32_t cp = wave_glyphs[0];

			if (ed->track.has_audio) {
				float pk = pcm_track_peak_in_ms(&ed->track,
				    b0, b1);
				int lvl = (int)lroundf(pk * 7.0f);

				if (lvl < 0) {
					lvl = 0;
				} else if (lvl > 7) {
					lvl = 7;
				}
				cp = wave_glyphs[lvl];
				cst.fg = tui_color_idx(lvl > 5 ? 11 : 10);
			}
			if (is_mark) {
				cp = 0x25c6; /* ◆ */
				cst.fg = tui_color_idx(14);
				cst.bold = 1;
			}
			if (in_sel) {
				cst.bg = tui_color_idx(4);
				cst.fg = tui_color_idx(15);
			}
			if (i == head_col) {
				cp = 0x2503; /* ┃ */
				cst.fg = tui_color_idx(
				    ed->mode == MODE_DUB ? 9 : 15);
				cst.bold = 1;
			}
			tui_surface_put_cp(ed->sf, tl_row, 1 + i, cp, &cst);
		}
		tui_surface_put_cp(ed->sf, tl_row, 1 + bar_w, ']', &bar_bg);
	}
	if (rhs_len > 0) {
		struct tui_style rst = {
			.fg = tui_color_idx(ed->mode == MODE_DUB ? 9 : 15),
			.bg = tui_color_idx(0),
			.bold = 1,
		};

		tui_surface_put_str(ed->sf, tl_row, ed->outer_cols - rhs_len,
		    rhs_len, &rst, time_rhs);
	}

	/* Bottom status / prompt row */
	int st_row = vp_rows + 1;
	if (st_row < ed->outer_rows) {
		struct tui_style st_bg = {
			.fg = tui_color_idx(15),
			.bg = tui_color_idx(0),
		};

		tui_surface_fill(ed->sf, st_row, 0, 1, ed->outer_cols, ' ',
		    &st_bg);
		if (ed->prompt.kind != PROMPT_NONE) {
			static const char *labels[] = {
				[PROMPT_JUMP] = "Jump (s or MM:SS): ",
				[PROMPT_FIND] = "Find text: ",
				[PROMPT_REDACT_NEEDLE] = "Redact search: ",
				[PROMPT_REDACT_REPL] = "Replace with: ",
				[PROMPT_TITLE] = "Title: ",
				[PROMPT_MARKER_LABEL] = "Marker label: ",
				[PROMPT_PAUSE_SEC] = "Insert pause (s, def 1.0): ",
				[PROMPT_IDLE_MAX_SEC] = "Max idle gap (s, def 1.0): ",
				[PROMPT_SAVE_AUDIO] = "Save audio path: ",
				[PROMPT_QUIT_CONFIRM] = "Unsaved edits! Quit without saving? [y/N]: ",
			};
			char line[320];
			struct tui_style pst = {
				.fg = tui_color_idx(14),
				.bg = tui_color_idx(0),
				.bold = 1,
			};

			snprintf(line, sizeof line, "%s%s_",
			    labels[ed->prompt.kind], ed->prompt.buf);
			tui_surface_put_str(ed->sf, st_row, 0, ed->outer_cols,
			    &pst, line);
		} else if (*ed->status != '\0') {
			tui_surface_put_str(ed->sf, st_row, 0, ed->outer_cols,
			    &st_bg, ed->status);
		} else {
			struct tui_style dim = {
				.fg = tui_color_idx(8),
				.bg = tui_color_idx(0),
			};

			tui_surface_put_str(ed->sf, st_row, 0, ed->outer_cols,
			    &dim,
			    "Space Play  h/l Scrub  i/o Mark  x Cut  m Mute  s Idle  V/O Dub  P Pause  u Undo  w Save  ? Help  q Quit");
		}
	}

	if (ed->show_help) {
		render_help_overlay(ed->sf, vp_rows, vp_cols);
	}

	int cr = 0, cc = 0, cvis = 1, cshape = 0;

	cast_player_get_cursor(ed->player, &cr, &cc, &cvis, &cshape);
	cr -= ed->pan_row;
	cc -= ed->pan_col;
	if (cr < 0 || cc < 0 || cr >= vp_rows || cc >= vp_cols) {
		cvis = 0;
	}
	tui_surface_flush(ed->sf, cr, cc, cvis, cshape);
}

static void
handle_key(struct edit_ctx *ed, int key)
{
	struct edit_prompt *pr = &ed->prompt;

	if (pr->kind != PROMPT_NONE) {
		if (key == KEY_ESC) {
			close_prompt(ed);
			set_status(ed, "Cancelled");
		} else if (key == KEY_ENTER || key == '\n') {
			submit_prompt(ed);
		} else if (key == KEY_BS || key == 0x08) {
			if (pr->len > 0) {
				pr->buf[--pr->len] = '\0';
			}
		} else if (key >= 0x20 && key < 0x7f) {
			if (pr->kind == PROMPT_QUIT_CONFIRM) {
				pr->buf[0] = (char)key;
				pr->buf[1] = '\0';
				submit_prompt(ed);
			} else if (pr->len + 1 < sizeof pr->buf) {
				pr->buf[pr->len++] = (char)key;
				pr->buf[pr->len] = '\0';
			}
		}
		return;
	}

	if (ed->show_help) {
		if (key == '?' || key == KEY_ESC || key == 'q') {
			ed->show_help = 0;
		}
		return;
	}

	if (ed->mode == MODE_DUB) {
		switch (key) {
		case ' ':
			ed->dub.frozen = !ed->dub.frozen;
			if (!ed->dub.frozen) {
				cast_player_invalidate(ed->player);
			}
			set_status(ed, ed->dub.frozen ?
			    "DUB FROZEN (Space resumes cast; . pulls next event)" :
			    "DUB LIVE (Space freezes frame)");
			break;
		case '.':
			dub_step_next_event(ed);
			break;
		case 'k':
			(void)cast_doc_add_marker(&ed->doc, ed->cur_ms, "");
			sync_hud_markers(ed);
			hud_note_marker(&ed->hud, ed->cur_ms);
			break;
		case 'V':
		case 'O':
		case KEY_ENTER:
		case '\n':
			finish_dub(ed, 1);
			break;
		case KEY_ESC:
			finish_dub(ed, 0);
			break;
		default:
			break;
		}
		return;
	}

	switch (key) {
	case 'q':
		if (ed->doc.dirty) {
			open_prompt(ed, PROMPT_QUIT_CONFIRM, NULL);
		} else {
			ed->running = 0;
		}
		break;
	case 'Q':
		ed->running = 0;
		break;
	case '?':
		ed->show_help = 1;
		break;
	case '\t':
		ed->hud_override = side_hud_visible(ed) ? -1 : 1;
		set_status(ed, side_hud_visible(ed) ?
		    "Side HUD shown" : "Side HUD hidden");
		break;
	case KEY_UP:
		ed->pan_row--;
		break;
	case KEY_DOWN:
		ed->pan_row++;
		break;
	case ' ':
		if (ed->mode == MODE_PLAY) {
			stop_playback(ed);
			set_status(ed, "Paused");
		} else {
			start_playback(ed);
		}
		break;
	case KEY_LEFT:
	case 'h':
		stop_playback(ed);
		seek_to(ed, ed->cur_ms - 500.0);
		break;
	case KEY_RIGHT:
	case 'l':
		stop_playback(ed);
		seek_to(ed, ed->cur_ms + 500.0);
		break;
	case KEY_PGUP:
	case 'H':
	case '[':
		stop_playback(ed);
		seek_to(ed, ed->cur_ms - 5000.0);
		break;
	case KEY_PGDN:
	case 'L':
	case ']':
		stop_playback(ed);
		seek_to(ed, ed->cur_ms + 5000.0);
		break;
	case ',':
		step_term_event(ed, -1);
		break;
	case '.':
		step_term_event(ed, 1);
		break;
	case KEY_HOME:
	case '0':
		stop_playback(ed);
		seek_to(ed, 0.0);
		break;
	case KEY_END:
	case '$':
		stop_playback(ed);
		seek_to(ed, total_timeline_ms(ed));
		break;
	case 'n': {
		size_t idx = 0;

		if (cast_doc_find_marker(&ed->doc, ed->cur_ms, 1, &idx) == 0) {
			stop_playback(ed);
			seek_to(ed, ed->doc.events[idx].t_ms);
			snprintf(ed->status, sizeof ed->status,
			    "Marker @ %.2fs %s", ed->cur_ms / 1000.0,
			    ed->doc.events[idx].data);
		} else {
			set_status(ed, "No next marker");
		}
		break;
	}
	case 'p': {
		size_t idx = 0;

		if (cast_doc_find_marker(&ed->doc, ed->cur_ms, -1, &idx) == 0) {
			stop_playback(ed);
			seek_to(ed, ed->doc.events[idx].t_ms);
			snprintf(ed->status, sizeof ed->status,
			    "Marker @ %.2fs %s", ed->cur_ms / 1000.0,
			    ed->doc.events[idx].data);
		} else {
			set_status(ed, "No previous marker");
		}
		break;
	}
	case '+':
	case '=':
	case '>': {
		int was_playing = (ed->mode == MODE_PLAY);

		stop_playback(ed);
		ed->speed += 0.25;
		if (ed->speed > 4.0) {
			ed->speed = 4.0;
		}
		if (was_playing) {
			start_playback(ed);
		}
		snprintf(ed->status, sizeof ed->status, "Speed: %.2fx",
		    ed->speed);
		break;
	}
	case '-':
	case '<': {
		int was_playing = (ed->mode == MODE_PLAY);

		stop_playback(ed);
		ed->speed -= 0.25;
		if (ed->speed < 0.25) {
			ed->speed = 0.25;
		}
		if (was_playing) {
			start_playback(ed);
		}
		snprintf(ed->status, sizeof ed->status, "Speed: %.2fx",
		    ed->speed);
		break;
	}
	case 'M':
		pcm_engine_set_play_mute(ed->eng,
		    !pcm_engine_get_play_mute(ed->eng));
		set_status(ed, pcm_engine_get_play_mute(ed->eng) ?
		    "Monitor muted" : "Monitor unmuted");
		break;
	case 'i':
		ed->sel.have_in = 1;
		ed->sel.in_ms = ed->cur_ms;
		snprintf(ed->status, sizeof ed->status, "Marked In @ %.2fs",
		    ed->cur_ms / 1000.0);
		break;
	case 'o':
		ed->sel.have_out = 1;
		ed->sel.out_ms = ed->cur_ms;
		snprintf(ed->status, sizeof ed->status, "Marked Out @ %.2fs",
		    ed->cur_ms / 1000.0);
		break;
	case 'c':
	case KEY_ESC:
		ed->sel.have_in = ed->sel.have_out = 0;
		set_status(ed, "Selection cleared");
		break;
	case 'x':
	case KEY_DEL:
		do_cut_selection(ed);
		break;
	case 'm':
		do_mute_selection(ed);
		break;
	case 'k':
		if (undo_save_current(ed, "marker") == 0) {
			(void)cast_doc_add_marker(&ed->doc, ed->cur_ms, "");
			sync_hud_markers(ed);
			hud_note_marker(&ed->hud, ed->cur_ms);
			snprintf(ed->status, sizeof ed->status,
			    "Added marker @ %.2fs", ed->cur_ms / 1000.0);
		}
		break;
	case 'K': {
		ssize_t midx = find_marker_near(&ed->doc, ed->cur_ms, 50.0);

		ed->prompt.marker_idx = midx;
		open_prompt(ed, PROMPT_MARKER_LABEL,
		    midx >= 0 ? ed->doc.events[midx].data : NULL);
		break;
	}
	case 'd': {
		ssize_t midx = find_marker_near(&ed->doc, ed->cur_ms, 100.0);

		if (midx < 0) {
			set_status(ed, "No marker near playhead");
		} else if (undo_save_current(ed, "delete marker") == 0) {
			double mt = ed->doc.events[midx].t_ms;

			(void)cast_doc_remove_event(&ed->doc, (size_t)midx);
			sync_hud_markers(ed);
			snprintf(ed->status, sizeof ed->status,
			    "Deleted marker at %.2fs", mt / 1000.0);
		}
		break;
	}
	case 'P':
		open_prompt(ed, PROMPT_PAUSE_SEC, NULL);
		break;
	case 's':
		open_prompt(ed, PROMPT_IDLE_MAX_SEC, NULL);
		break;
	case '(':
		do_nudge_audio(ed, -50.0);
		break;
	case ')':
		do_nudge_audio(ed, 50.0);
		break;
	case 'V':
		start_dub(ed, 1);
		break;
	case 'O':
		start_dub(ed, 0);
		break;
	case ':':
	case 'g':
		open_prompt(ed, PROMPT_JUMP, NULL);
		break;
	case '/':
		open_prompt(ed, PROMPT_FIND, NULL);
		break;
	case 'R':
		open_prompt(ed, PROMPT_REDACT_NEEDLE, NULL);
		break;
	case 'T':
		open_prompt(ed, PROMPT_TITLE, ed->doc.title);
		break;
	case 'u':
	case KEY_CTRL_Z:
		perform_undo(ed, 0);
		break;
	case 'U':
	case KEY_CTRL_R:
		perform_undo(ed, 1);
		break;
	case 'w':
		do_save(ed, NULL);
		break;
	default:
		break;
	}
}

static void
tick_editor(struct edit_ctx *ed)
{
	struct audio_vu vu;

	memset(&vu, 0, sizeof vu);
	if (ed->mode == MODE_PLAY) {
		double tot = total_timeline_ms(ed);

		if (ed->track.has_audio) {
			if (!pcm_engine_is_playing(ed->eng)) {
				ed->cur_ms = tot;
				pcm_engine_stop_play(ed->eng);
				ed->mode = MODE_PAUSE;
				hud_note_pause(&ed->hud, 1);
			} else {
				ed->cur_ms = pcm_engine_play_pos_ms(ed->eng);
			}
			pcm_engine_get_vu(ed->eng, &vu);
		} else {
			double dt = wall_elapsed_ms(&ed->last_wall) * ed->speed;

			ed->cur_ms += dt;
			if (ed->cur_ms >= tot) {
				ed->cur_ms = tot;
				ed->mode = MODE_PAUSE;
				hud_note_pause(&ed->hud, 1);
			}
		}
		cast_player_seek(ed->player, ed->cur_ms);
		follow_cursor_pan(ed);
		if (ed->cur_ms >= tot) {
			set_status(ed, "Reached end");
		}
	} else if (ed->mode == MODE_DUB) {
		double rec_ms = pcm_engine_dub_ms(ed->eng);
		double dt = rec_ms - ed->dub.prev_rec_ms;

		pcm_engine_get_vu(ed->eng, &vu);
		if (dt > 0.0) {
			if (ed->dub.frozen) {
				(void)cast_doc_insert_pause(&ed->doc,
				    ed->cur_ms, dt);
				(void)pcm_track_insert_silence(&ed->track,
				    ed->cur_ms, dt);
				ed->cur_ms += dt;
			} else {
				ed->cur_ms += dt;
				if (ed->cur_ms > ed->doc.duration_ms) {
					ed->doc.duration_ms = ed->cur_ms;
					ed->doc.dirty = 1;
				}
				cast_player_seek(ed->player, ed->cur_ms);
				follow_cursor_pan(ed);
			}
			ed->dub.prev_rec_ms = rec_ms;
		}
	}

	clamp_cur_ms(ed);
	hud_tick(&ed->hud, ed->cur_ms, &vu);
}

int
edit_main(int argc, char **argv)
{
	struct edit_ctx *ed;
	const char *audio_opt = NULL;
	const char *in_dev = NULL, *out_dev = NULL;
	enum audio_format afmt = AUDIO_FMT_AUTO;
	int use_raw = 0;
	int ch;
	extern char *optarg;
	extern int optind;
	char err[256];
	struct termios rtt;
	struct sigaction sa, old_winch;

	while ((ch = getopt(argc, argv, "?a:d:F:hlo:R")) != EOF) {
		switch (ch) {
		case 'a':
			audio_opt = optarg;
			break;
		case 'd':
			in_dev = optarg;
			break;
		case 'F':
			if (audio_parse_format(optarg, &afmt) != 0) {
				fprintf(stderr,
				    "castty: unsupported audio format: %s\n",
				    optarg);
				edit_usage(EXIT_FAILURE);
			}
			break;
		case 'l':
			audio_list_outputs();
			break;
		case 'o':
			out_dev = optarg;
			break;
		case 'R':
			use_raw = 1;
			break;
		case 'h':
		case '?':
			edit_usage(EXIT_SUCCESS);
			break;
		default:
			edit_usage(EXIT_FAILURE);
			break;
		}
	}

	argc -= optind;
	argv += optind;
	if (argc < 1) {
		fprintf(stderr, "castty edit: missing <file.cast> argument\n");
		edit_usage(EXIT_FAILURE);
	}

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fprintf(stderr,
		    "castty edit requires an interactive terminal\n");
		return 1;
	}

	ed = calloc(1, sizeof *ed);
	if (ed == NULL) {
		fprintf(stderr, "castty edit: out of memory\n");
		return 1;
	}
	ed->cast_path = argv[0];
	ed->audio_path = audio_opt ? strdup(audio_opt) : NULL;
	ed->audio_fmt = afmt;
	ed->in_devid = in_dev;
	ed->out_devid = out_dev;
	ed->use_raw = use_raw;
	ed->speed = 1.0;
	ed->ungot_byte = -1;
	ed->prompt.marker_idx = -1;
	ed->running = 1;
	pcm_track_init(&ed->track);

	if (cast_doc_load(ed->cast_path, &ed->doc, err, sizeof err) != 0) {
		fprintf(stderr, "%s\n", err);
		free(ed->audio_path);
		free(ed);
		return 1;
	}

	if (ed->audio_path != NULL) {
		struct stat st;

		if (stat(ed->audio_path, &st) == 0 && st.st_size > 0) {
			if (pcm_track_load(&ed->track, ed->audio_path,
			    ed->audio_fmt, ed->doc.duration_ms, err,
			    sizeof err) != 0) {
				fprintf(stderr, "%s\n", err);
				cast_doc_free(&ed->doc);
				free(ed->audio_path);
				free(ed);
				return 1;
			}
		} else {
			pcm_track_create_empty(&ed->track, ed->doc.duration_ms);
		}
	} else {
		pcm_track_create_empty(&ed->track, ed->doc.duration_ms);
	}

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &win) == -1) {
		win.ws_row = 24;
		win.ws_col = 80;
	}
	owin = rwin = win;
	ed->outer_rows = win.ws_row > 0 ? win.ws_row : 24;
	ed->outer_cols = win.ws_col > 0 ? win.ws_col : 80;
	ed->player = cast_player_create(&ed->doc);
	ed->eng = pcm_engine_create(&ed->track, ed->in_devid, ed->out_devid,
	    ed->use_raw);
	ed->sf = tui_surface_create(ed->outer_rows, ed->outer_cols,
	    STDOUT_FILENO);

	enum audio_format eff_fmt = AUDIO_FMT_AUTO;

	if (ed->audio_path != NULL) {
		eff_fmt = (ed->audio_fmt != AUDIO_FMT_AUTO) ?
		    ed->audio_fmt : audio_infer_format(ed->audio_path);
	}
	hud_init(&ed->hud, 1, 0, ed->track.has_audio || ed->in_devid != NULL,
	    (ed->audio_path != NULL) ? audio_format_label(eff_fmt) : "none", 0,
	    0.0);
	sync_hud_markers(ed);

	xtcgetattr(STDIN_FILENO, &tt);
	term_reset_init(getenv("TERM"), STDOUT_FILENO);
	tty_saved = 1;
	rtt = tt;
	rtt.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|INLCR|IGNCR|ICRNL|IXON);
	rtt.c_oflag &= ~OPOST;
	rtt.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	rtt.c_cflag &= ~(CSIZE|PARENB);
	rtt.c_cflag |= CS8;
	rtt.c_cc[VMIN] = 1;
	rtt.c_cc[VTIME] = 0;
	xtcsetattr(STDIN_FILENO, TCSAFLUSH, &rtt);

	memset(&sa, 0, sizeof sa);
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = on_edit_sigwinch;
	xsigaction(SIGWINCH, &sa, &old_winch);

	(void)tui_surface_write_raw(ed->sf, "\x1b[2J\x1b[H", 7);
	cast_player_seek(ed->player, 0.0);
	render_editor(ed);

	while (ed->running) {
		if (edit_winched) {
			edit_winched = 0;
			if (ioctl(STDIN_FILENO, TIOCGWINSZ, &rwin) == 0 &&
			    rwin.ws_row > 0 && rwin.ws_col > 0) {
				ed->outer_rows = rwin.ws_row;
				ed->outer_cols = rwin.ws_col;
				tui_surface_resize(ed->sf, ed->outer_rows,
				    ed->outer_cols);
			}
		}

		int key = read_key(ed, STDIN_FILENO, 50);

		if (key != KEY_NONE) {
			handle_key(ed, key);
		}
		tick_editor(ed);
		if (ed->running) {
			render_editor(ed);
		}
	}

	stop_playback(ed);
	if (ed->mode == MODE_DUB) {
		finish_dub(ed, 0);
	}
	(void)tui_surface_write_raw(ed->sf, "\x1b[0m\x1b[2J\x1b[H\x1b[?25h", 17);
	xsigaction(SIGWINCH, &old_winch, NULL);
	restore_tty();

	for (size_t i = 0; i < ed->undo.len; i++) {
		undo_entry_free(&ed->undo.entries[i]);
	}
	if (ed->dub.has_snap) {
		undo_entry_free(&ed->dub.snap);
	}
	tui_surface_destroy(ed->sf);
	pcm_engine_destroy(ed->eng);
	pcm_track_destroy(&ed->track);
	cast_player_destroy(ed->player);
	cast_doc_free(&ed->doc);
	free(ed->audio_path);
	free(ed);
	return 0;
}

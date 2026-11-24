#ifndef TUI_SCREEN_H
#define TUI_SCREEN_H

#include <stddef.h>
#include <stdint.h>
#include <vterm.h>

struct tui_style {
	uint8_t bold;
	uint8_t italic;
	uint8_t underline;
	uint8_t reverse;
	uint8_t strike;
	VTermColor fg;
	VTermColor bg;
};

struct tui_surface;
struct tui_vterm;

typedef void (*tui_vterm_reply_fn)(const char *bytes, size_t len, void *user);

static inline VTermColor
tui_color_default_fg(void)
{
	VTermColor c = { .type = VTERM_COLOR_DEFAULT_FG };
	return c;
}

static inline VTermColor
tui_color_default_bg(void)
{
	VTermColor c = { .type = VTERM_COLOR_DEFAULT_BG };
	return c;
}

static inline VTermColor
tui_color_idx(uint8_t idx)
{
	VTermColor c;
	vterm_color_indexed(&c, idx);
	return c;
}

static inline VTermColor
tui_color_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	VTermColor c;
	vterm_color_rgb(&c, r, g, b);
	return c;
}

static inline struct tui_style
tui_style_default(void)
{
	struct tui_style st = {
		.fg = tui_color_default_fg(),
		.bg = tui_color_default_bg(),
	};
	return st;
}

static inline int
tui_vterm_decscusr(int cursor_shape, int cursor_blink)
{
	if (cursor_shape <= 0) {
		return 0;
	}
	switch (cursor_shape) {
	case VTERM_PROP_CURSORSHAPE_BLOCK:
		return cursor_blink ? 1 : 2;
	case VTERM_PROP_CURSORSHAPE_UNDERLINE:
		return cursor_blink ? 3 : 4;
	case VTERM_PROP_CURSORSHAPE_BAR_LEFT:
		return cursor_blink ? 5 : 6;
	default:
		return 0;
	}
}

struct tui_surface *tui_surface_create(int rows, int cols, int outfd);
void tui_surface_destroy(struct tui_surface *sf);
void tui_surface_resize(struct tui_surface *sf, int rows, int cols);
void tui_surface_invalidate(struct tui_surface *sf);
void tui_surface_begin(struct tui_surface *sf);
void tui_surface_put_cell(struct tui_surface *sf, int row, int col,
    const VTermScreenCell *cell);
void tui_surface_put_cp(struct tui_surface *sf, int row, int col, uint32_t cp,
    const struct tui_style *st);
void tui_surface_put_str(struct tui_surface *sf, int row, int col,
    int max_cols, const struct tui_style *st, const char *utf8_str);
void tui_surface_fill(struct tui_surface *sf, int row, int col, int nrows,
    int ncols, uint32_t cp, const struct tui_style *st);
void tui_surface_flush(struct tui_surface *sf, int cursor_row, int cursor_col,
    int cursor_visible, int cursor_shape);
int tui_surface_write_raw(struct tui_surface *sf, const char *bytes,
    size_t len);

struct tui_vterm *tui_vterm_create(int rows, int cols,
    tui_vterm_reply_fn reply_fn, void *reply_user);
void tui_vterm_destroy(struct tui_vterm *tv);
void tui_vterm_resize(struct tui_vterm *tv, int rows, int cols);
void tui_vterm_feed(struct tui_vterm *tv, const char *bytes, size_t len,
    int fwd_fd);
void tui_vterm_blit(struct tui_vterm *tv, struct tui_surface *sf, int dst_row,
    int dst_col);
int tui_vterm_take_dirty(struct tui_vterm *tv);
void tui_vterm_get_cursor(const struct tui_vterm *tv, int *row, int *col,
    int *visible, int *shape);
int tui_vterm_take_title(struct tui_vterm *tv, char *buf, size_t sz);

#endif

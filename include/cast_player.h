#ifndef CAST_PLAYER_H
#define CAST_PLAYER_H

#include <stddef.h>

#include "cast_doc.h"
#include "tui/screen.h"

struct cast_player;

struct cast_player *cast_player_create(const struct cast_doc *doc);
void cast_player_destroy(struct cast_player *pl);

/* Rebuild cached checkpoints after doc edits. */
void cast_player_invalidate(struct cast_player *pl);

/* Seek cleanly to target_ms using nearest prior checkpoint + libvterm replay. */
int cast_player_seek(struct cast_player *pl, double target_ms);

int cast_player_cols(const struct cast_player *pl);
int cast_player_rows(const struct cast_player *pl);
double cast_player_pos_ms(const struct cast_player *pl);

/* Blit cast terminal region starting at (src_row, src_col) into sf at
 * (dst_row, dst_col), clipped to (max_rows, max_cols). */
void cast_player_blit(const struct cast_player *pl, struct tui_surface *sf,
    int src_row, int src_col, int dst_row, int dst_col, int max_rows,
    int max_cols);
void cast_player_get_cursor(const struct cast_player *pl, int *row, int *col,
    int *visible, int *shape);

/* Synthesize standalone ANSI snapshot at at_ms (e.g. to keep screen state
 * consistent across a [t0, t1] cut). Returns 0 on success. */
int cast_player_snapshot_ansi(const struct cast_doc *doc, double at_ms,
    char **out_buf, size_t *out_len, int *cols_out, int *rows_out);

#endif

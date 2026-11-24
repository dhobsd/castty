#ifndef CAST_DOC_H
#define CAST_DOC_H

#include <stddef.h>

enum cast_ev_type {
	CAST_EV_OUT = 0,
	CAST_EV_IN,
	CAST_EV_RESIZE,
	CAST_EV_MARKER,
	CAST_EV_EXIT,
};

struct cast_event {
	double t_ms;
	enum cast_ev_type type;
	char *data;
	size_t len;
	int cols;
	int rows;
};

struct cast_cut_gap {
	double t0_ms;
	double t1_ms;
};

struct cast_doc {
	int version;
	int cols;
	int rows;
	long long timestamp;
	char *title;
	char *cmd;
	char *term_type;
	char *term_version;
	char *theme_json;
	char *env_json;
	double duration_ms;

	struct cast_event *events;
	size_t nevents;
	size_t cap;
	int dirty;
};

void cast_doc_init(struct cast_doc *doc);
void cast_doc_free(struct cast_doc *doc);
int cast_doc_clone(const struct cast_doc *src, struct cast_doc *dst);

int cast_doc_load(const char *path, struct cast_doc *doc, char *errbuf,
    size_t errsz);
int cast_doc_save(const struct cast_doc *doc, const char *path, char *errbuf,
    size_t errsz);

int cast_doc_insert_event(struct cast_doc *doc, double t_ms,
    enum cast_ev_type type, const char *data, size_t len, int cols, int rows);
int cast_doc_insert_pause(struct cast_doc *doc, double at_ms, double add_ms);
int cast_doc_cut_range(struct cast_doc *doc, double t0_ms, double t1_ms,
    const char *snap_data, size_t snap_len, int snap_cols, int snap_rows);
int cast_doc_compress_idle(struct cast_doc *doc, double t0_ms, double t1_ms,
    double max_gap_ms, struct cast_cut_gap **gaps_out, size_t *ngaps_out,
    double *saved_ms_out);

int cast_doc_add_marker(struct cast_doc *doc, double t_ms, const char *label);
int cast_doc_set_marker_label(struct cast_doc *doc, size_t ev_idx,
    const char *label);
int cast_doc_remove_event(struct cast_doc *doc, size_t ev_idx);
size_t cast_doc_redact(struct cast_doc *doc, const char *needle,
    const char *repl);
void cast_doc_set_title(struct cast_doc *doc, const char *title);
void cast_doc_nudge(struct cast_doc *doc, double shift_ms);

int cast_doc_find_marker(const struct cast_doc *doc, double from_ms, int dir,
    size_t *idx_out);
int cast_doc_find_text(const struct cast_doc *doc, const char *needle,
    double from_ms, int dir, size_t *idx_out);

#endif

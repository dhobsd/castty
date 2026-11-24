#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cast_doc.h"
#include "json.h"

struct dynbuf {
	char *data;
	size_t len;
	size_t cap;
};

static int
dbuf_reserve(struct dynbuf *b, size_t extra)
{
	size_t need = b->len + extra;

	if (need <= b->cap) {
		return 0;
	}
	size_t ncap = b->cap ? b->cap * 2 : 128;
	while (ncap < need) {
		ncap *= 2;
	}
	char *np = realloc(b->data, ncap);
	if (np == NULL) {
		return -1;
	}
	b->data = np;
	b->cap = ncap;
	return 0;
}

static int
dbuf_push_bytes(struct dynbuf *b, const char *s, size_t n)
{
	if (n == 0) {
		return 0;
	}
	if (dbuf_reserve(b, n + 1) != 0) {
		return -1;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
	return 0;
}

static int
dbuf_push_byte(struct dynbuf *b, unsigned char c)
{
	return dbuf_push_bytes(b, (const char *)&c, 1);
}

static char *
dup_span(const char *a, const char *b)
{
	size_t n = (size_t)(b - a);
	char *s = malloc(n + 1);

	if (s != NULL) {
		memcpy(s, a, n);
		s[n] = '\0';
	}
	return s;
}

static char *
dup_mem(const char *s, size_t len)
{
	char *p = malloc(len + 1);

	if (p != NULL) {
		if (len > 0) {
			memcpy(p, s, len);
		}
		p[len] = '\0';
	}
	return p;
}

void
cast_doc_init(struct cast_doc *doc)
{
	memset(doc, 0, sizeof *doc);
	doc->version = 3;
	doc->cols = 80;
	doc->rows = 24;
}

void
cast_doc_free(struct cast_doc *doc)
{
	if (doc == NULL) {
		return;
	}
	free(doc->title);
	free(doc->cmd);
	free(doc->term_type);
	free(doc->term_version);
	free(doc->theme_json);
	free(doc->env_json);
	for (size_t i = 0; i < doc->nevents; i++) {
		free(doc->events[i].data);
	}
	free(doc->events);
	memset(doc, 0, sizeof *doc);
}

int
cast_doc_clone(const struct cast_doc *src, struct cast_doc *dst)
{
	cast_doc_init(dst);
	dst->version = src->version;
	dst->cols = src->cols;
	dst->rows = src->rows;
	dst->timestamp = src->timestamp;
	dst->duration_ms = src->duration_ms;
	dst->dirty = src->dirty;

	if ((src->title && !(dst->title = strdup(src->title))) ||
	    (src->cmd && !(dst->cmd = strdup(src->cmd))) ||
	    (src->term_type && !(dst->term_type = strdup(src->term_type))) ||
	    (src->term_version &&
	    !(dst->term_version = strdup(src->term_version))) ||
	    (src->theme_json && !(dst->theme_json = strdup(src->theme_json))) ||
	    (src->env_json && !(dst->env_json = strdup(src->env_json)))) {
		cast_doc_free(dst);
		return -1;
	}

	if (src->nevents > 0) {
		dst->events = calloc(src->nevents, sizeof *dst->events);
		if (dst->events == NULL) {
			cast_doc_free(dst);
			return -1;
		}
		dst->cap = src->nevents;
		for (size_t i = 0; i < src->nevents; i++) {
			dst->events[i] = src->events[i];
			dst->events[i].data = dup_mem(src->events[i].data,
			    src->events[i].len);
			if (dst->events[i].data == NULL) {
				dst->nevents = i;
				cast_doc_free(dst);
				return -1;
			}
		}
		dst->nevents = src->nevents;
	}
	return 0;
}

static int
append_event_slot(struct cast_doc *doc, size_t idx, double t_ms,
    enum cast_ev_type type, const char *data, size_t len, int cols, int rows)
{
	char *copy;

	if (doc->nevents == doc->cap) {
		size_t ncap = doc->cap ? doc->cap * 2 : 64;
		struct cast_event *ne = realloc(doc->events, ncap * sizeof *ne);

		if (ne == NULL) {
			return -1;
		}
		doc->events = ne;
		doc->cap = ncap;
	}

	copy = dup_mem(data ? data : "", len);
	if (copy == NULL) {
		return -1;
	}

	if (idx < doc->nevents) {
		memmove(&doc->events[idx + 1], &doc->events[idx],
		    (doc->nevents - idx) * sizeof *doc->events);
	}
	doc->events[idx] = (struct cast_event){
		.t_ms = t_ms,
		.type = type,
		.data = copy,
		.len = len,
		.cols = cols,
		.rows = rows,
	};
	doc->nevents++;
	if (t_ms > doc->duration_ms) {
		doc->duration_ms = t_ms;
	}
	return 0;
}

int
cast_doc_insert_event(struct cast_doc *doc, double t_ms, enum cast_ev_type type,
    const char *data, size_t len, int cols, int rows)
{
	size_t idx = doc->nevents;

	if (t_ms < 0.0) {
		t_ms = 0.0;
	}
	while (idx > 0 && doc->events[idx - 1].t_ms > t_ms) {
		idx--;
	}
	if (append_event_slot(doc, idx, t_ms, type, data, len, cols, rows) != 0) {
		return -1;
	}
	doc->dirty = 1;
	return 0;
}

static const char *
parse_term_obj(const char *p, const char *end, struct cast_doc *doc)
{
	p = json_skip_ws(p, end);
	if (p >= end || *p != '{') {
		return NULL;
	}
	p = json_skip_ws(p + 1, end);
	if (p < end && *p == '}') {
		return p + 1;
	}
	for (;;) {
		char key[64];

		p = json_parse_string_buf(p, end, key, sizeof key);
		if (p == NULL) {
			return NULL;
		}
		p = json_skip_ws(p, end);
		if (p >= end || *p != ':') {
			return NULL;
		}
		p = json_skip_ws(p + 1, end);

		if (strcmp(key, "cols") == 0) {
			int v = 0;

			p = json_parse_int(p, end, &v);
			if (p != NULL && v > 0) {
				doc->cols = v;
			}
		} else if (strcmp(key, "rows") == 0) {
			int v = 0;

			p = json_parse_int(p, end, &v);
			if (p != NULL && v > 0) {
				doc->rows = v;
			}
		} else if (strcmp(key, "type") == 0) {
			free(doc->term_type);
			doc->term_type = NULL;
			p = json_parse_string(p, end, &doc->term_type, NULL);
		} else if (strcmp(key, "version") == 0) {
			free(doc->term_version);
			doc->term_version = NULL;
			p = json_parse_string(p, end, &doc->term_version, NULL);
		} else if (strcmp(key, "theme") == 0 && p < end && *p == '{') {
			const char *vstart = p;

			p = json_skip_object(p, end);
			if (p != NULL) {
				free(doc->theme_json);
				doc->theme_json = dup_span(vstart, p);
			}
		} else {
			p = json_skip_value(p, end);
		}
		if (p == NULL) {
			return NULL;
		}
		p = json_skip_ws(p, end);
		if (p >= end) {
			return NULL;
		}
		if (*p == '}') {
			return p + 1;
		}
		if (*p != ',') {
			return NULL;
		}
		p++;
	}
}

static const char *
parse_v1_stdout_array(const char *p, const char *end, struct cast_doc *doc,
    double *acc_ms_out)
{
	double acc_ms = 0.0;

	p = json_skip_ws(p, end);
	if (p >= end || *p != '[') {
		return NULL;
	}
	p = json_skip_ws(p + 1, end);
	if (p < end && *p == ']') {
		*acc_ms_out = 0.0;
		return p + 1;
	}
	for (;;) {
		double dt = 0.0;
		char *str = NULL;
		size_t slen = 0;

		p = json_skip_ws(p, end);
		if (p >= end || *p != '[') {
			return NULL;
		}
		p = json_parse_double(p + 1, end, &dt);
		if (p == NULL) {
			return NULL;
		}
		p = json_skip_ws(p, end);
		if (p >= end || *p != ',') {
			return NULL;
		}
		p = json_parse_string(p + 1, end, &str, &slen);
		if (p == NULL) {
			return NULL;
		}
		p = json_skip_ws(p, end);
		if (p >= end || *p != ']') {
			free(str);
			return NULL;
		}
		p++;

		if (dt > 0.0) {
			acc_ms += dt * 1000.0;
		}
		if (slen > 0 || dt > 0.0) {
			int rc = append_event_slot(doc, doc->nevents, acc_ms,
			    CAST_EV_OUT, str, slen, 0, 0);

			free(str);
			if (rc != 0) {
				return NULL;
			}
		} else {
			free(str);
		}

		p = json_skip_ws(p, end);
		if (p >= end) {
			return NULL;
		}
		if (*p == ']') {
			*acc_ms_out = acc_ms;
			return p + 1;
		}
		if (*p != ',') {
			return NULL;
		}
		p++;
	}
}

static int
parse_ndjson_body(const char *p, const char *end, struct cast_doc *doc)
{
	double acc_ms = 0.0;

	while (p < end) {
		double t = 0.0;
		char code[16];
		char *data = NULL;
		size_t dlen = 0;
		enum cast_ev_type etype;
		int cols = 0, rows = 0;

		p = json_skip_ws(p, end);
		if (p >= end) {
			break;
		}
		if (*p == '#') {
			while (p < end && *p != '\n') {
				p++;
			}
			continue;
		}
		if (*p != '[') {
			return -1;
		}
		p = json_parse_double(p + 1, end, &t);
		if (p == NULL) {
			return -1;
		}
		p = json_skip_ws(p, end);
		if (p >= end || *p != ',') {
			return -1;
		}
		p = json_parse_string_buf(p + 1, end, code, sizeof code);
		if (p == NULL) {
			return -1;
		}
		p = json_skip_ws(p, end);
		if (p >= end || *p != ',') {
			return -1;
		}
		p = json_skip_ws(p + 1, end);
		if (p < end && *p == '"') {
			p = json_parse_string(p, end, &data, &dlen);
		} else {
			p = json_skip_value(p, end);
		}
		if (p == NULL) {
			return -1;
		}
		for (;;) {
			p = json_skip_ws(p, end);
			if (p >= end) {
				free(data);
				return -1;
			}
			if (*p == ']') {
				p++;
				break;
			}
			if (*p != ',') {
				free(data);
				return -1;
			}
			p = json_skip_value(p + 1, end);
			if (p == NULL) {
				free(data);
				return -1;
			}
		}

		if (doc->version == 3) {
			if (t > 0.0) {
				acc_ms += t * 1000.0;
			}
		} else {
			double abs_ms = t * 1000.0;

			if (abs_ms > acc_ms) {
				acc_ms = abs_ms;
			}
		}

		if (strcmp(code, "o") == 0) {
			etype = CAST_EV_OUT;
		} else if (strcmp(code, "i") == 0) {
			etype = CAST_EV_IN;
		} else if (strcmp(code, "r") == 0) {
			etype = CAST_EV_RESIZE;
			if (data != NULL) {
				(void)sscanf(data, "%dx%d", &cols, &rows);
			}
		} else if (strcmp(code, "m") == 0) {
			etype = CAST_EV_MARKER;
		} else if (strcmp(code, "x") == 0) {
			etype = CAST_EV_EXIT;
		} else {
			free(data);
			continue;
		}

		if (append_event_slot(doc, doc->nevents, acc_ms, etype, data,
		    dlen, cols, rows) != 0) {
			free(data);
			return -1;
		}
		free(data);
	}

	if (acc_ms > doc->duration_ms) {
		doc->duration_ms = acc_ms;
	}
	return 0;
}

int
cast_doc_load(const char *path, struct cast_doc *doc, char *errbuf,
    size_t errsz)
{
	FILE *fp;
	struct stat st;
	char *buf;
	const char *p, *end;
	double top_dur_sec = -1.0, v1_acc_ms = 0.0;
	int version = 0;

	cast_doc_init(doc);
	fp = fopen(path, "rb");
	if (fp == NULL) {
		snprintf(errbuf, errsz, "castty: cannot open %s: %s", path,
		    strerror(errno));
		return -1;
	}
	if (fstat(fileno(fp), &st) != 0 || st.st_size <= 0) {
		snprintf(errbuf, errsz, "castty: %s is empty", path);
		fclose(fp);
		return -1;
	}
	buf = malloc((size_t)st.st_size + 1);
	if (buf == NULL) {
		snprintf(errbuf, errsz, "castty: out of memory reading %s",
		    path);
		fclose(fp);
		return -1;
	}
	if (fread(buf, 1, (size_t)st.st_size, fp) != (size_t)st.st_size) {
		snprintf(errbuf, errsz, "castty: failed reading %s", path);
		free(buf);
		fclose(fp);
		return -1;
	}
	buf[st.st_size] = '\0';
	fclose(fp);

	p = buf;
	end = buf + st.st_size;
	if ((size_t)st.st_size >= 3 && (unsigned char)p[0] == 0xef &&
	    (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf) {
		p += 3;
	}
	p = json_skip_ws(p, end);
	if (p >= end || *p != '{') {
		snprintf(errbuf, errsz, "castty: %s is not valid JSON", path);
		free(buf);
		return -1;
	}

	p = json_skip_ws(p + 1, end);
	if (p < end && *p != '}') {
		for (;;) {
			char key[64];

			p = json_parse_string_buf(p, end, key, sizeof key);
			if (p == NULL) {
				goto fail_fmt;
			}
			p = json_skip_ws(p, end);
			if (p >= end || *p != ':') {
				goto fail_fmt;
			}
			p = json_skip_ws(p + 1, end);

			if (strcmp(key, "version") == 0) {
				p = json_parse_int(p, end, &version);
			} else if (strcmp(key, "width") == 0) {
				int v = 0;

				p = json_parse_int(p, end, &v);
				if (p != NULL && v > 0) {
					doc->cols = v;
				}
			} else if (strcmp(key, "height") == 0) {
				int v = 0;

				p = json_parse_int(p, end, &v);
				if (p != NULL && v > 0) {
					doc->rows = v;
				}
			} else if (strcmp(key, "timestamp") == 0) {
				double d;

				p = json_parse_double(p, end, &d);
				if (p != NULL) {
					doc->timestamp = (long long)d;
				}
			} else if (strcmp(key, "duration") == 0) {
				p = json_parse_double(p, end, &top_dur_sec);
			} else if (strcmp(key, "title") == 0) {
				free(doc->title);
				doc->title = NULL;
				p = json_parse_string(p, end, &doc->title, NULL);
			} else if (strcmp(key, "command") == 0) {
				free(doc->cmd);
				doc->cmd = NULL;
				p = json_parse_string(p, end, &doc->cmd, NULL);
			} else if (strcmp(key, "env") == 0 && p < end && *p == '{') {
				const char *vs = p;

				p = json_skip_object(p, end);
				if (p != NULL) {
					free(doc->env_json);
					doc->env_json = dup_span(vs, p);
				}
			} else if (strcmp(key, "theme") == 0 && p < end &&
			    *p == '{') {
				const char *vs = p;

				p = json_skip_object(p, end);
				if (p != NULL) {
					free(doc->theme_json);
					doc->theme_json = dup_span(vs, p);
				}
			} else if (strcmp(key, "term") == 0) {
				p = parse_term_obj(p, end, doc);
			} else if (strcmp(key, "stdout") == 0) {
				p = parse_v1_stdout_array(p, end, doc,
				    &v1_acc_ms);
			} else {
				p = json_skip_value(p, end);
			}
			if (p == NULL) {
				goto fail_fmt;
			}
			p = json_skip_ws(p, end);
			if (p >= end) {
				goto fail_fmt;
			}
			if (*p == '}') {
				p++;
				break;
			}
			if (*p != ',') {
				goto fail_fmt;
			}
			p = json_skip_ws(p + 1, end);
		}
	} else if (p < end && *p == '}') {
		p++;
	}

	if (version < 1 || version > 3) {
		goto fail_fmt;
	}
	doc->version = version;

	if (version == 1) {
		p = json_skip_ws(p, end);
		if (p != end) {
			goto fail_fmt;
		}
		if (top_dur_sec >= 0.0 && top_dur_sec * 1000.0 > v1_acc_ms) {
			doc->duration_ms = top_dur_sec * 1000.0;
		} else {
			doc->duration_ms = v1_acc_ms;
		}
	} else {
		if (parse_ndjson_body(p, end, doc) != 0) {
			goto fail_fmt;
		}
	}

	free(buf);
	doc->dirty = 0;
	return 0;

fail_fmt:
	snprintf(errbuf, errsz, "castty: %s is not a valid asciicast file",
	    path);
	free(buf);
	cast_doc_free(doc);
	return -1;
}

int
cast_doc_save(const struct cast_doc *doc, const char *path, char *errbuf,
    size_t errsz)
{
	size_t tlen = strlen(path) + sizeof(".castty.XXXXXX");
	char *tmp = malloc(tlen);
	int fd, v1_first = 1;
	FILE *fp;
	int64_t written_ticks = 0, dur_ticks;

	if (tmp == NULL) {
		snprintf(errbuf, errsz, "castty: out of memory saving %s", path);
		return -1;
	}
	snprintf(tmp, tlen, "%s.castty.XXXXXX", path);
	fd = mkstemp(tmp);
	if (fd < 0) {
		snprintf(errbuf, errsz, "castty: cannot create temp for %s: %s",
		    path, strerror(errno));
		free(tmp);
		return -1;
	}
	(void)fchmod(fd, 0644);
	fp = fdopen(fd, "wb");
	if (fp == NULL) {
		snprintf(errbuf, errsz, "castty: fdopen failed: %s",
		    strerror(errno));
		close(fd);
		unlink(tmp);
		free(tmp);
		return -1;
	}

	if (doc->version == 3) {
		fprintf(fp, "{\"version\": 3, \"term\": {\"cols\": %d, \"rows\": %d",
		    doc->cols, doc->rows);
		if (doc->term_type && *doc->term_type) {
			fputs(", \"type\": ", fp);
			json_write_escaped(fp, doc->term_type,
			    strlen(doc->term_type));
		}
		if (doc->term_version && *doc->term_version) {
			fputs(", \"version\": ", fp);
			json_write_escaped(fp, doc->term_version,
			    strlen(doc->term_version));
		}
		if (doc->theme_json && *doc->theme_json) {
			fprintf(fp, ", \"theme\": %s", doc->theme_json);
		}
		fprintf(fp, "}, \"timestamp\": %lld", doc->timestamp);
		if (doc->cmd && *doc->cmd) {
			fputs(", \"command\": ", fp);
			json_write_escaped(fp, doc->cmd, strlen(doc->cmd));
		}
		if (doc->title && *doc->title) {
			fputs(", \"title\": ", fp);
			json_write_escaped(fp, doc->title, strlen(doc->title));
		}
		if (doc->env_json && *doc->env_json) {
			fprintf(fp, ", \"env\": %s", doc->env_json);
		}
		fputs("}\n", fp);
	} else {
		fprintf(fp, "{\"version\": %d, \"width\": %d, \"height\": %d",
		    doc->version, doc->cols, doc->rows);
		if (doc->timestamp > 0) {
			fprintf(fp, ", \"timestamp\": %lld", doc->timestamp);
		}
		fputs(", \"command\": ", fp);
		json_write_escaped(fp, doc->cmd ? doc->cmd : "",
		    doc->cmd ? strlen(doc->cmd) : 0);
		fputs(", \"title\": ", fp);
		json_write_escaped(fp, doc->title ? doc->title : "",
		    doc->title ? strlen(doc->title) : 0);
		if (doc->env_json && *doc->env_json) {
			fprintf(fp, ", \"env\": %s", doc->env_json);
		}
		if (doc->version == 2 && doc->theme_json && *doc->theme_json) {
			fprintf(fp, ", \"theme\": %s", doc->theme_json);
		}
		if (doc->version == 2) {
			fputs("}\n", fp);
		} else {
			fputs(", \"stdout\": [\n", fp);
		}
	}

	for (size_t i = 0; i < doc->nevents; i++) {
		const struct cast_event *ev = &doc->events[i];
		int64_t tgt_ticks = (int64_t)llround(ev->t_ms * 10.0);
		int64_t dt_ticks;

		if (tgt_ticks < written_ticks) {
			tgt_ticks = written_ticks;
		}
		dt_ticks = tgt_ticks - written_ticks;
		if (doc->version == 1) {
			if (ev->type == CAST_EV_OUT) {
				fprintf(fp, "%s[%.4f,", v1_first ? "" : ",",
				    (double)dt_ticks / 10000.0);
				json_write_escaped(fp, ev->data, ev->len);
				fputs("]\n", fp);
				v1_first = 0;
				written_ticks = tgt_ticks;
			}
		} else {
			static const char *codes[] = { "o", "i", "r", "m", "x" };
			int64_t out_ticks = (doc->version == 3) ?
			    dt_ticks : tgt_ticks;

			fprintf(fp, "[%.4f,\"%s\",",
			    (double)out_ticks / 10000.0, codes[ev->type]);
			json_write_escaped(fp, ev->data, ev->len);
			fputs("]\n", fp);
			written_ticks = tgt_ticks;
		}
	}

	dur_ticks = (int64_t)llround(doc->duration_ms * 10.0);
	if (dur_ticks > written_ticks + 5) {
		if (doc->version == 2) {
			fprintf(fp, "[%.4f,\"o\",\"\"]\n",
			    (double)dur_ticks / 10000.0);
		} else if (doc->version == 3) {
			fprintf(fp, "[%.4f,\"o\",\"\"]\n",
			    (double)(dur_ticks - written_ticks) / 10000.0);
		}
	}

	if (doc->version == 1) {
		fprintf(fp, "], \"duration\": %.9g}\n",
		    doc->duration_ms / 1000.0);
	}

	if (ferror(fp) || fflush(fp) != 0 || fsync(fileno(fp)) != 0 ||
	    fclose(fp) != 0) {
		snprintf(errbuf, errsz, "castty: write error saving %s", path);
		unlink(tmp);
		free(tmp);
		return -1;
	}
	if (rename(tmp, path) != 0) {
		snprintf(errbuf, errsz, "castty: rename failed for %s: %s",
		    path, strerror(errno));
		unlink(tmp);
		free(tmp);
		return -1;
	}
	free(tmp);
	return 0;
}

int
cast_doc_insert_pause(struct cast_doc *doc, double at_ms, double add_ms)
{
	if (add_ms <= 0.0) {
		return 0;
	}
	for (size_t i = 0; i < doc->nevents; i++) {
		if (doc->events[i].t_ms > at_ms) {
			doc->events[i].t_ms += add_ms;
		}
	}
	doc->duration_ms += add_ms;
	doc->dirty = 1;
	return 0;
}

int
cast_doc_cut_range(struct cast_doc *doc, double t0_ms, double t1_ms,
    const char *snap_data, size_t snap_len, int snap_cols, int snap_rows)
{
	double cut_ms;
	size_t w = 0, ins_pos = 0;
	int need_snap_resize = 0;

	if (t0_ms < 0.0) {
		t0_ms = 0.0;
	}
	if (t1_ms > doc->duration_ms) {
		t1_ms = doc->duration_ms;
	}
	if (t1_ms <= t0_ms) {
		return 0;
	}
	cut_ms = t1_ms - t0_ms;

	/* Detect whether dimensions changed across the deleted span. */
	int cols_t0 = doc->cols, rows_t0 = doc->rows;
	for (size_t i = 0; i < doc->nevents; i++) {
		if (doc->events[i].t_ms <= t0_ms) {
			if (doc->events[i].type == CAST_EV_RESIZE &&
			    doc->events[i].cols > 0 && doc->events[i].rows > 0) {
				cols_t0 = doc->events[i].cols;
				rows_t0 = doc->events[i].rows;
			}
			ins_pos = i + 1;
		} else {
			break;
		}
	}
	if (snap_cols > 0 && snap_rows > 0 &&
	    (snap_cols != cols_t0 || snap_rows != rows_t0)) {
		need_snap_resize = 1;
	}

	for (size_t i = 0; i < doc->nevents; i++) {
		if (doc->events[i].t_ms > t0_ms && doc->events[i].t_ms <= t1_ms) {
			free(doc->events[i].data);
			continue;
		}
		doc->events[w] = doc->events[i];
		if (doc->events[w].t_ms > t1_ms) {
			doc->events[w].t_ms -= cut_ms;
		}
		w++;
	}
	doc->nevents = w;
	if (ins_pos > doc->nevents) {
		ins_pos = doc->nevents;
	}

	if (need_snap_resize) {
		char rbuf[32];
		int n = snprintf(rbuf, sizeof rbuf, "%dx%d", snap_cols,
		    snap_rows);

		if (n > 0 && (size_t)n < sizeof rbuf) {
			if (append_event_slot(doc, ins_pos, t0_ms,
			    CAST_EV_RESIZE, rbuf, (size_t)n, snap_cols,
			    snap_rows) != 0) {
				return -1;
			}
			ins_pos++;
		}
	}

	if (snap_data != NULL && snap_len > 0) {
		if (append_event_slot(doc, ins_pos, t0_ms, CAST_EV_OUT,
		    snap_data, snap_len, 0, 0) != 0) {
			return -1;
		}
	}

	doc->duration_ms -= cut_ms;
	if (doc->duration_ms < 0.0) {
		doc->duration_ms = 0.0;
	}
	doc->dirty = 1;
	return 0;
}

static int
push_cut_gap(struct cast_cut_gap **gaps, size_t *ngaps, size_t *gcap,
    double t0_ms, double t1_ms)
{
	if (*ngaps == *gcap) {
		size_t nc = *gcap ? *gcap * 2 : 16;
		struct cast_cut_gap *ng = realloc(*gaps, nc * sizeof *ng);

		if (ng == NULL) {
			return -1;
		}
		*gaps = ng;
		*gcap = nc;
	}
	(*gaps)[(*ngaps)++] = (struct cast_cut_gap){
		.t0_ms = t0_ms,
		.t1_ms = t1_ms,
	};
	return 0;
}

int
cast_doc_compress_idle(struct cast_doc *doc, double t0_ms, double t1_ms,
    double max_gap_ms, struct cast_cut_gap **gaps_out, size_t *ngaps_out,
    double *saved_ms_out)
{
	struct cast_cut_gap *gaps = NULL;
	size_t ngaps = 0, gcap = 0;
	double total_saved = 0.0;
	double prev_orig = 0.0;

	if (max_gap_ms < 10.0) {
		max_gap_ms = 10.0;
	}

	for (size_t i = 0; i < doc->nevents; i++) {
		double orig = doc->events[i].t_ms;
		double dt = orig - prev_orig;

		if (orig >= t0_ms && orig <= t1_ms && dt > max_gap_ms) {
			double cut = dt - max_gap_ms;
			double gap_start = prev_orig + max_gap_ms - total_saved;

			if (push_cut_gap(&gaps, &ngaps, &gcap, gap_start,
			    gap_start + cut) != 0) {
				free(gaps);
				return -1;
			}
			total_saved += cut;
		}
		doc->events[i].t_ms = orig - total_saved;
		prev_orig = orig;
	}

	if (doc->duration_ms >= t0_ms && doc->duration_ms <= t1_ms &&
	    doc->duration_ms - prev_orig > max_gap_ms) {
		double cut = (doc->duration_ms - prev_orig) - max_gap_ms;
		double gap_start = prev_orig + max_gap_ms - total_saved;

		if (push_cut_gap(&gaps, &ngaps, &gcap, gap_start,
		    gap_start + cut) != 0) {
			free(gaps);
			return -1;
		}
		total_saved += cut;
	}
	doc->duration_ms -= total_saved;
	if (doc->duration_ms < 0.0) {
		doc->duration_ms = 0.0;
	}
	if (total_saved > 0.0) {
		doc->dirty = 1;
	}

	if (gaps_out != NULL) {
		*gaps_out = gaps;
	} else {
		free(gaps);
	}
	if (ngaps_out != NULL) {
		*ngaps_out = ngaps;
	}
	if (saved_ms_out != NULL) {
		*saved_ms_out = total_saved;
	}
	return 0;
}

int
cast_doc_add_marker(struct cast_doc *doc, double t_ms, const char *label)
{
	const char *s = label ? label : "";

	return cast_doc_insert_event(doc, t_ms, CAST_EV_MARKER, s, strlen(s),
	    0, 0);
}

int
cast_doc_set_marker_label(struct cast_doc *doc, size_t ev_idx,
    const char *label)
{
	char *copy;
	size_t n;

	if (ev_idx >= doc->nevents ||
	    doc->events[ev_idx].type != CAST_EV_MARKER) {
		return -1;
	}
	n = label ? strlen(label) : 0;
	copy = dup_mem(label ? label : "", n);
	if (copy == NULL) {
		return -1;
	}
	free(doc->events[ev_idx].data);
	doc->events[ev_idx].data = copy;
	doc->events[ev_idx].len = n;
	doc->dirty = 1;
	return 0;
}

int
cast_doc_remove_event(struct cast_doc *doc, size_t ev_idx)
{
	if (ev_idx >= doc->nevents) {
		return -1;
	}
	free(doc->events[ev_idx].data);
	if (ev_idx + 1 < doc->nevents) {
		memmove(&doc->events[ev_idx], &doc->events[ev_idx + 1],
		    (doc->nevents - ev_idx - 1) * sizeof *doc->events);
	}
	doc->nevents--;
	doc->dirty = 1;
	return 0;
}

static size_t
redact_stream_type(struct cast_doc *doc, enum cast_ev_type stype,
    const char *needle, size_t nlen, const char *repl, size_t rlen)
{
	size_t total = 0, off = 0, pos = 0, count = 0;
	char *flat = NULL, *repl_ch = NULL;
	uint8_t *act = NULL;

	for (size_t i = 0; i < doc->nevents; i++) {
		if (doc->events[i].type == stype) {
			total += doc->events[i].len;
		}
	}
	if (total < nlen) {
		return 0;
	}
	flat = malloc(total);
	act = calloc(total, 1);
	repl_ch = calloc(total, 1);
	if (flat == NULL || act == NULL || repl_ch == NULL) {
		free(flat);
		free(act);
		free(repl_ch);
		return 0;
	}

	for (size_t i = 0; i < doc->nevents; i++) {
		if (doc->events[i].type == stype && doc->events[i].len > 0) {
			memcpy(flat + off, doc->events[i].data,
			    doc->events[i].len);
			off += doc->events[i].len;
		}
	}

	while (pos + nlen <= total) {
		if (memcmp(flat + pos, needle, nlen) == 0) {
			for (size_t j = 0; j < nlen; j++) {
				if (j + 1 < nlen) {
					if (j < rlen) {
						act[pos + j] = 1;
						repl_ch[pos + j] = repl[j];
					} else {
						act[pos + j] = 3;
					}
				} else if (j < rlen) {
					act[pos + j] = 2;
				} else {
					act[pos + j] = 3;
				}
			}
			pos += nlen;
			count++;
		} else {
			pos++;
		}
	}

	if (count > 0) {
		off = 0;
		for (size_t i = 0; i < doc->nevents; i++) {
			struct cast_event *ev = &doc->events[i];
			int modified = 0;

			if (ev->type != stype) {
				continue;
			}
			for (size_t k = 0; k < ev->len; k++) {
				if (act[off + k] != 0) {
					modified = 1;
					break;
				}
			}
			if (modified) {
				struct dynbuf b = { 0 };
				int err = 0;

				for (size_t k = 0; k < ev->len; k++) {
					uint8_t a = act[off + k];

					if (a == 0) {
						err |= dbuf_push_byte(&b,
						    (unsigned char)ev->data[k]);
					} else if (a == 1) {
						err |= dbuf_push_byte(&b,
						    (unsigned char)repl_ch[off + k]);
					} else if (a == 2) {
						size_t rem = rlen - (nlen - 1);

						err |= dbuf_push_bytes(&b,
						    repl + (nlen - 1), rem);
					}
				}
				if (!err && dbuf_reserve(&b, 1) == 0) {
					b.data[b.len] = '\0';
					free(ev->data);
					ev->data = b.data;
					ev->len = b.len;
					doc->dirty = 1;
				} else {
					free(b.data);
				}
			}
			off += ev->len;
		}
	}

	free(flat);
	free(act);
	free(repl_ch);
	return count;
}

size_t
cast_doc_redact(struct cast_doc *doc, const char *needle, const char *repl)
{
	size_t nlen, rlen;

	if (needle == NULL || *needle == '\0') {
		return 0;
	}
	nlen = strlen(needle);
	rlen = repl ? strlen(repl) : 0;

	return redact_stream_type(doc, CAST_EV_OUT, needle, nlen,
	    repl ? repl : "", rlen) +
	    redact_stream_type(doc, CAST_EV_IN, needle, nlen,
	    repl ? repl : "", rlen);
}

void
cast_doc_set_title(struct cast_doc *doc, const char *title)
{
	free(doc->title);
	doc->title = (title && *title) ? strdup(title) : NULL;
	doc->dirty = 1;
}

void
cast_doc_nudge(struct cast_doc *doc, double shift_ms)
{
	double max_t = 0.0;

	for (size_t i = 0; i < doc->nevents; i++) {
		doc->events[i].t_ms += shift_ms;
		if (doc->events[i].t_ms < 0.0) {
			doc->events[i].t_ms = 0.0;
		}
		if (doc->events[i].t_ms > max_t) {
			max_t = doc->events[i].t_ms;
		}
	}
	doc->duration_ms += shift_ms;
	if (doc->duration_ms < max_t) {
		doc->duration_ms = max_t;
	}
	if (doc->duration_ms < 0.0) {
		doc->duration_ms = 0.0;
	}
	doc->dirty = 1;
}

int
cast_doc_find_marker(const struct cast_doc *doc, double from_ms, int dir,
    size_t *idx_out)
{
	if (dir >= 0) {
		for (size_t i = 0; i < doc->nevents; i++) {
			if (doc->events[i].type == CAST_EV_MARKER &&
			    doc->events[i].t_ms > from_ms + 5.0) {
				if (idx_out != NULL) {
					*idx_out = i;
				}
				return 0;
			}
		}
	} else {
		for (size_t i = doc->nevents; i > 0; i--) {
			size_t idx = i - 1;

			if (doc->events[idx].type == CAST_EV_MARKER &&
			    doc->events[idx].t_ms < from_ms - 5.0) {
				if (idx_out != NULL) {
					*idx_out = idx;
				}
				return 0;
			}
		}
	}
	return -1;
}

static const char *
find_submem(const char *hay, size_t hlen, const char *needle, size_t nlen)
{
	if (nlen == 0) {
		return hay;
	}
	if (hay == NULL || needle == NULL || hlen < nlen) {
		return NULL;
	}
	for (size_t i = 0; i <= hlen - nlen; i++) {
		if (hay[i] == needle[0] &&
		    memcmp(hay + i, needle, nlen) == 0) {
			return hay + i;
		}
	}
	return NULL;
}

int
cast_doc_find_text(const struct cast_doc *doc, const char *needle,
    double from_ms, int dir, size_t *idx_out)
{
	size_t nlen, raw_total = 0, vis_len = 0;
	char *vis = NULL;
	size_t *vis_ev = NULL;
	int strip_ansi, found = 0;
	size_t best_idx = 0;
	enum { ESC_NONE = 0, ESC_SEEN, ESC_CSI, ESC_OSC, ESC_OSC_ESC } st =
	    ESC_NONE;

	if (needle == NULL || *needle == '\0') {
		return -1;
	}
	nlen = strlen(needle);
	strip_ansi = (memchr(needle, 0x1b, nlen) == NULL);

	for (size_t i = 0; i < doc->nevents; i++) {
		if (doc->events[i].type == CAST_EV_OUT) {
			raw_total += doc->events[i].len;
		} else if (doc->events[i].type == CAST_EV_MARKER &&
		    doc->events[i].len >= nlen &&
		    find_submem(doc->events[i].data, doc->events[i].len,
		    needle, nlen) != NULL) {
			double t = doc->events[i].t_ms;

			if (dir >= 0 && t > from_ms + 1.0 &&
			    (!found || i < best_idx)) {
				best_idx = i;
				found = 1;
			} else if (dir < 0 && t < from_ms - 1.0 &&
			    (!found || i > best_idx)) {
				best_idx = i;
				found = 1;
			}
		}
	}

	if (raw_total >= nlen) {
		vis = malloc(raw_total);
		vis_ev = malloc(raw_total * sizeof *vis_ev);
		if (vis != NULL && vis_ev != NULL) {
			for (size_t i = 0; i < doc->nevents; i++) {
				const struct cast_event *ev = &doc->events[i];

				if (ev->type != CAST_EV_OUT) {
					continue;
				}
				for (size_t k = 0; k < ev->len; k++) {
					unsigned char c =
					    (unsigned char)ev->data[k];

					if (!strip_ansi) {
						vis[vis_len] = (char)c;
						vis_ev[vis_len++] = i;
						continue;
					}
					switch (st) {
					case ESC_NONE:
						if (c == 0x1b) {
							st = ESC_SEEN;
						} else {
							vis[vis_len] = (char)c;
							vis_ev[vis_len++] = i;
						}
						break;
					case ESC_SEEN:
						if (c == '[') {
							st = ESC_CSI;
						} else if (c == ']') {
							st = ESC_OSC;
						} else {
							st = ESC_NONE;
						}
						break;
					case ESC_CSI:
						if (c >= 0x40 && c <= 0x7e) {
							st = ESC_NONE;
						}
						break;
					case ESC_OSC:
						if (c == 0x07) {
							st = ESC_NONE;
						} else if (c == 0x1b) {
							st = ESC_OSC_ESC;
						}
						break;
					case ESC_OSC_ESC:
						st = (c == '\\') ?
						    ESC_NONE : ESC_OSC;
						break;
					}
				}
			}

			if (vis_len >= nlen) {
				for (size_t p = 0; p + nlen <= vis_len; p++) {
					if (vis[p] == needle[0] &&
					    memcmp(vis + p, needle, nlen) == 0) {
						size_t cand =
						    vis_ev[p + nlen - 1];
						double t =
						    doc->events[cand].t_ms;

						if (dir >= 0 &&
						    t > from_ms + 1.0) {
							if (!found ||
							    cand < best_idx) {
								best_idx = cand;
								found = 1;
							}
							break;
						}
						if (dir < 0 &&
						    t < from_ms - 1.0 &&
						    (!found ||
						    cand > best_idx)) {
							best_idx = cand;
							found = 1;
						}
					}
				}
			}
		}
		free(vis);
		free(vis_ev);
	}

	if (found) {
		if (idx_out != NULL) {
			*idx_out = best_idx;
		}
		return 0;
	}
	return -1;
}

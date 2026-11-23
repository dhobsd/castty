#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cast.h"

static const char *
skip_ws(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
		p++;
	}
	return p;
}

static const char *
skip_string(const char *p, const char *end)
{
	if (p >= end || *p != '"') {
		return NULL;
	}
	p++;
	while (p < end) {
		if (*p == '\\') {
			p++;
			if (p >= end) {
				return NULL;
			}
			p++;
		} else if (*p == '"') {
			return p + 1;
		} else {
			p++;
		}
	}
	return NULL;
}

static const char *
parse_string(const char *p, const char *end, char *out, size_t cap)
{
	size_t o = 0;

	if (p >= end || *p != '"') {
		return NULL;
	}
	p++;
	while (p < end) {
		if (*p == '"') {
			if (cap > 0) {
				out[o < cap ? o : cap - 1] = '\0';
			}
			return p + 1;
		}
		if (*p == '\\') {
			p++;
			if (p >= end) {
				return NULL;
			}
			if (o + 1 < cap) {
				out[o++] = *p;
			}
			p++;
		} else {
			if (o + 1 < cap) {
				out[o++] = *p;
			}
			p++;
		}
	}
	return NULL;
}

static const char *
parse_double_tok(const char *p, const char *end, double *val)
{
	char buf[64], *eptr;
	size_t n = 0;

	p = skip_ws(p, end);
	if (p >= end) {
		return NULL;
	}
	while (p < end && n + 1 < sizeof buf &&
	    (isdigit((unsigned char)*p) || *p == '+' || *p == '-' ||
	     *p == '.' || *p == 'e' || *p == 'E')) {
		buf[n++] = *p++;
	}
	if (n == 0) {
		return NULL;
	}
	buf[n] = '\0';
	errno = 0;
	*val = strtod(buf, &eptr);
	if (errno == ERANGE || eptr == buf || *eptr != '\0') {
		return NULL;
	}
	return p;
}

static const char *
parse_int_tok(const char *p, const char *end, int *val)
{
	double d;
	const char *next = parse_double_tok(p, end, &d);

	if (next == NULL || d < (double)INT_MIN || d > (double)INT_MAX) {
		return NULL;
	}
	*val = (int)d;
	return next;
}

static const char *skip_value(const char *p, const char *end);

static const char *
skip_array(const char *p, const char *end)
{
	p = skip_ws(p, end);
	if (p >= end || *p != '[') {
		return NULL;
	}
	p = skip_ws(p + 1, end);
	if (p < end && *p == ']') {
		return p + 1;
	}
	for (;;) {
		p = skip_value(p, end);
		if (p == NULL) {
			return NULL;
		}
		p = skip_ws(p, end);
		if (p >= end) {
			return NULL;
		}
		if (*p == ']') {
			return p + 1;
		}
		if (*p != ',') {
			return NULL;
		}
		p++;
	}
}

static const char *
skip_object(const char *p, const char *end)
{
	p = skip_ws(p, end);
	if (p >= end || *p != '{') {
		return NULL;
	}
	p = skip_ws(p + 1, end);
	if (p < end && *p == '}') {
		return p + 1;
	}
	for (;;) {
		p = skip_string(skip_ws(p, end), end);
		if (p == NULL) {
			return NULL;
		}
		p = skip_ws(p, end);
		if (p >= end || *p != ':') {
			return NULL;
		}
		p = skip_value(p + 1, end);
		if (p == NULL) {
			return NULL;
		}
		p = skip_ws(p, end);
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
skip_value(const char *p, const char *end)
{
	p = skip_ws(p, end);
	if (p >= end) {
		return NULL;
	}
	if (*p == '"') {
		return skip_string(p, end);
	}
	if (*p == '{') {
		return skip_object(p, end);
	}
	if (*p == '[') {
		return skip_array(p, end);
	}
	if (*p == '-' || isdigit((unsigned char)*p)) {
		double d;

		return parse_double_tok(p, end, &d);
	}
	if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
		return p + 4;
	}
	if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
		return p + 5;
	}
	if (end - p >= 4 && memcmp(p, "null", 4) == 0) {
		return p + 4;
	}
	return NULL;
}

static int
parse_term_obj(const char *p, const char *end, const char **next_out,
    int *cols, int *rows)
{
	p = skip_ws(p, end);
	if (p >= end || *p != '{') {
		return -1;
	}
	p = skip_ws(p + 1, end);
	if (p < end && *p == '}') {
		*next_out = p + 1;
		return 0;
	}
	for (;;) {
		char key[64];

		p = parse_string(skip_ws(p, end), end, key, sizeof key);
		if (p == NULL) {
			return -1;
		}
		p = skip_ws(p, end);
		if (p >= end || *p != ':') {
			return -1;
		}
		p = skip_ws(p + 1, end);
		if (strcmp(key, "cols") == 0) {
			p = parse_int_tok(p, end, cols);
		} else if (strcmp(key, "rows") == 0) {
			p = parse_int_tok(p, end, rows);
		} else {
			p = skip_value(p, end);
		}
		if (p == NULL) {
			return -1;
		}
		p = skip_ws(p, end);
		if (p >= end) {
			return -1;
		}
		if (*p == '}') {
			*next_out = p + 1;
			return 0;
		}
		if (*p != ',') {
			return -1;
		}
		p++;
	}
}

static int
parse_v1_stdout(const char *p, const char *end, const char **rbracket_out,
    double *sum_sec, int *elem_count)
{
	p = skip_ws(p, end);
	if (p >= end || *p != '[') {
		return -1;
	}
	p = skip_ws(p + 1, end);
	if (p < end && *p == ']') {
		*rbracket_out = p;
		return 0;
	}
	for (;;) {
		double dt = 0.0;

		p = skip_ws(p, end);
		if (p >= end || *p != '[') {
			return -1;
		}
		p = parse_double_tok(p + 1, end, &dt);
		if (p == NULL) {
			return -1;
		}
		p = skip_ws(p, end);
		if (p >= end || *p != ',') {
			return -1;
		}
		p = skip_value(p + 1, end);
		if (p == NULL) {
			return -1;
		}
		p = skip_ws(p, end);
		if (p >= end || *p != ']') {
			return -1;
		}
		p++;
		if (dt > 0.0) {
			*sum_sec += dt;
		}
		(*elem_count)++;

		p = skip_ws(p, end);
		if (p >= end) {
			return -1;
		}
		if (*p == ']') {
			*rbracket_out = p;
			return 0;
		}
		if (*p != ',') {
			return -1;
		}
		p++;
	}
}

static int
parse_ndjson_events(const char *p, const char *end, int version,
    int *cols, int *rows, int *markers_out, double *total_sec)
{
	double acc = 0.0;
	int markers = 0;

	while (p < end) {
		double t = 0.0;
		char code[16], payload[64];

		p = skip_ws(p, end);
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
		p = parse_double_tok(p + 1, end, &t);
		if (p == NULL) {
			return -1;
		}
		p = skip_ws(p, end);
		if (p >= end || *p != ',') {
			return -1;
		}
		p = parse_string(skip_ws(p + 1, end), end, code, sizeof code);
		if (p == NULL) {
			return -1;
		}
		p = skip_ws(p, end);
		if (p >= end || *p != ',') {
			return -1;
		}
		p = skip_ws(p + 1, end);
		if (strcmp(code, "m") == 0) {
			markers++;
		}
		if (strcmp(code, "r") == 0 && p < end && *p == '"') {
			int rc = 0, rr = 0;

			p = parse_string(p, end, payload, sizeof payload);
			if (p != NULL && sscanf(payload, "%dx%d", &rc, &rr) == 2 &&
			    rc > 0 && rr > 0) {
				*cols = rc;
				*rows = rr;
			}
		} else {
			p = skip_value(p, end);
		}
		if (p == NULL) {
			return -1;
		}
		for (;;) {
			p = skip_ws(p, end);
			if (p >= end) {
				return -1;
			}
			if (*p == ']') {
				p++;
				break;
			}
			if (*p != ',') {
				return -1;
			}
			p = skip_value(p + 1, end);
			if (p == NULL) {
				return -1;
			}
		}

		if (version == 3) {
			if (t > 0.0) {
				acc += t;
			}
		} else if (t > acc) {
			acc = t;
		}
	}

	if (markers_out != NULL) {
		*markers_out = markers;
	}
	*total_sec = acc;
	return 0;
}

static int
build_v1_rewrite(const char *obj_start, const char *stdout_key,
    const char *stdout_rbracket, const char *dur_start,
    const char *dur_end, const char *obj_end,
    struct cast_append_info *info)
{
	/*
	 * Normalize v1 JSON layout so that `"stdout":[...` ends cleanly at the
	 * final array closing bracket, with any `"duration"` removed and any
	 * trailing non-duration members moved before `"stdout"`.
	 */
	size_t cap = (size_t)(obj_end - obj_start) + 64;
	char *out = malloc(cap);
	size_t len = 0;
	const char *cursor = obj_start;
	const char *after_stdout = skip_ws(stdout_rbracket + 1, obj_end);
	const char *p;

	if (out == NULL) {
		return -1;
	}

	/* Emit everything from obj_start up to stdout_key, skipping dur_start..dur_end */
	if (dur_start != NULL && dur_end <= stdout_key) {
		size_t pre = (size_t)(dur_start - cursor);
		const char *rest = dur_end;

		while (pre > 0 && isspace((unsigned char)cursor[pre - 1])) {
			pre--;
		}
		if (pre > 0 && cursor[pre - 1] == ',') {
			pre--;
			memcpy(out + len, cursor, pre);
			len += pre;
			memcpy(out + len, rest, (size_t)(stdout_key - rest));
			len += (size_t)(stdout_key - rest);
		} else {
			rest = skip_ws(rest, stdout_key);
			if (rest < stdout_key && *rest == ',') {
				rest++;
			}
			memcpy(out + len, cursor, (size_t)(dur_start - cursor));
			len += (size_t)(dur_start - cursor);
			memcpy(out + len, rest, (size_t)(stdout_key - rest));
			len += (size_t)(stdout_key - rest);
		}
	} else {
		memcpy(out + len, cursor, (size_t)(stdout_key - cursor));
		len += (size_t)(stdout_key - cursor);
	}

	/* Append any trailing members after stdout_rbracket other than "duration" */
	p = after_stdout;
	while (p < obj_end && *p != '}') {
		const char *mem_start, *mem_end;
		char key[64];

		if (*p == ',') {
			p = skip_ws(p + 1, obj_end);
		}
		if (p >= obj_end || *p == '}') {
			break;
		}
		mem_start = p;
		p = parse_string(p, obj_end, key, sizeof key);
		if (p == NULL) {
			free(out);
			return -1;
		}
		p = skip_ws(p, obj_end);
		if (p >= obj_end || *p != ':') {
			free(out);
			return -1;
		}
		mem_end = skip_value(p + 1, obj_end);
		if (mem_end == NULL) {
			free(out);
			return -1;
		}
		if (strcmp(key, "duration") != 0) {
			memcpy(out + len, mem_start, (size_t)(mem_end - mem_start));
			len += (size_t)(mem_end - mem_start);
			out[len++] = ',';
		}
		p = skip_ws(mem_end, obj_end);
	}

	memcpy(out + len, stdout_key, (size_t)(stdout_rbracket - stdout_key));
	len += (size_t)(stdout_rbracket - stdout_key);

	info->v1_rewrite_buf = out;
	info->v1_rewrite_len = len;
	info->v1_truncate_off = -1;
	return 0;
}

int
cast_inspect(const char *path, int req_version, struct cast_append_info *info,
    char *errbuf, size_t errsz)
{
	FILE *fp;
	long fsz;
	char *buf;
	const char *p, *end, *obj_start, *obj_end = NULL;
	const char *stdout_key = NULL, *stdout_rbracket = NULL;
	const char *dur_start = NULL, *dur_end = NULL;
	int version = 0, cols = 80, rows = 24, stdout_elems = 0;
	int has_dur = 0, dur_after_other = 0;
	double top_dur_sec = 0.0, v1_sum_sec = 0.0, nd_total_sec = 0.0;

	memset(info, 0, sizeof *info);
	info->v1_truncate_off = -1;

	fp = fopen(path, "rb");
	if (fp == NULL) {
		if (errno == ENOENT) {
			return 0;
		}
		snprintf(errbuf, errsz, "castty: cannot open %s: %s", path,
		    strerror(errno));
		return -1;
	}
	if (fseek(fp, 0, SEEK_END) != 0 || (fsz = ftell(fp)) < 0) {
		snprintf(errbuf, errsz, "castty: cannot inspect %s", path);
		fclose(fp);
		return -1;
	}
	if (fsz == 0) {
		fclose(fp);
		return 0;
	}
	buf = malloc((size_t)fsz + 1);
	if (buf == NULL) {
		snprintf(errbuf, errsz, "castty: out of memory");
		fclose(fp);
		return -1;
	}
	rewind(fp);
	if (fread(buf, 1, (size_t)fsz, fp) != (size_t)fsz) {
		snprintf(errbuf, errsz, "castty: failed reading %s", path);
		free(buf);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	buf[fsz] = '\0';

	end = buf + fsz;
	p = skip_ws(buf, end);
	if (p >= end) {
		free(buf);
		return 0;
	}
	if (*p != '{') {
		snprintf(errbuf, errsz,
		    "castty: %s is not a valid asciicast file", path);
		free(buf);
		return -1;
	}

	obj_start = p;
	p = skip_ws(p + 1, end);
	if (p < end && *p != '}') {
		for (;;) {
			const char *key_start = skip_ws(p, end);
			const char *val_start;
			char key[64];

			p = parse_string(key_start, end, key, sizeof key);
			if (p == NULL) {
				obj_end = NULL;
				break;
			}
			p = skip_ws(p, end);
			if (p >= end || *p != ':') {
				obj_end = NULL;
				break;
			}
			val_start = skip_ws(p + 1, end);

			if (strcmp(key, "version") == 0) {
				p = parse_int_tok(val_start, end, &version);
			} else if (strcmp(key, "width") == 0) {
				p = parse_int_tok(val_start, end, &cols);
			} else if (strcmp(key, "height") == 0) {
				p = parse_int_tok(val_start, end, &rows);
			} else if (strcmp(key, "term") == 0) {
				if (parse_term_obj(val_start, end, &p, &cols,
				    &rows) != 0) {
					p = NULL;
				}
			} else if (strcmp(key, "duration") == 0) {
				dur_start = key_start;
				p = parse_double_tok(val_start, end, &top_dur_sec);
				dur_end = p;
				has_dur = (p != NULL);
				if (stdout_rbracket != NULL && dur_after_other) {
					dur_after_other = -1;
				}
			} else if (strcmp(key, "stdout") == 0) {
				stdout_key = key_start;
				if (parse_v1_stdout(val_start, end, &stdout_rbracket,
				    &v1_sum_sec, &stdout_elems) != 0) {
					p = NULL;
				} else {
					p = stdout_rbracket + 1;
				}
			} else {
				if (stdout_rbracket != NULL) {
					dur_after_other = 1;
				}
				p = skip_value(val_start, end);
			}

			if (p == NULL) {
				obj_end = NULL;
				break;
			}
			p = skip_ws(p, end);
			if (p >= end) {
				obj_end = NULL;
				break;
			}
			if (*p == '}') {
				obj_end = p + 1;
				break;
			}
			if (*p != ',') {
				obj_end = NULL;
				break;
			}
			p++;
		}
	} else if (p < end && *p == '}') {
		obj_end = p + 1;
	}

	if (obj_end == NULL || (version != 1 && version != 2 && version != 3)) {
		snprintf(errbuf, errsz,
		    "castty: %s is not a valid asciicast v1/v2/v3 file", path);
		free(buf);
		return -1;
	}
	if (req_version != 0 && req_version != version) {
		snprintf(errbuf, errsz,
		    "castty: %s is asciicast v%d, cannot append as v%d",
		    path, version, req_version);
		free(buf);
		return -1;
	}

	info->exists = 1;
	info->version = version;
	info->cols = cols > 0 ? cols : 80;
	info->rows = rows > 0 ? rows : 24;

	if (version == 1) {
		const char *tail = skip_ws(obj_end, end);

		if (stdout_rbracket == NULL || tail != end) {
			snprintf(errbuf, errsz,
			    "castty: %s has invalid asciicast v1 structure", path);
			free(buf);
			return -1;
		}
		info->v1_empty = (stdout_elems == 0);
		info->v1_events_ms = v1_sum_sec * 1000.0;
		info->dur_ms = ((has_dur && top_dur_sec > v1_sum_sec) ?
		    top_dur_sec : v1_sum_sec) * 1000.0;

		if ((dur_start != NULL && dur_start < stdout_key) ||
		    dur_after_other != 0) {
			if (build_v1_rewrite(obj_start, stdout_key,
			    stdout_rbracket, dur_start, dur_end, obj_end,
			    info) != 0) {
				snprintf(errbuf, errsz,
				    "castty: failed preparing v1 append for %s", path);
				free(buf);
				return -1;
			}
		} else {
			info->v1_truncate_off = (long long)(stdout_rbracket - buf);
		}
	} else {
		if (parse_ndjson_events(obj_end, end, version, &info->cols,
		    &info->rows, &info->existing_markers, &nd_total_sec) != 0) {
			snprintf(errbuf, errsz,
			    "castty: %s contains invalid asciicast v%d events",
			    path, version);
			free(buf);
			return -1;
		}
		info->dur_ms = nd_total_sec * 1000.0;
		info->need_newline = (buf[fsz - 1] != '\n');
	}

	free(buf);
	return 0;
}

void
cast_append_info_free(struct cast_append_info *info)
{
	if (info != NULL) {
		free(info->v1_rewrite_buf);
		info->v1_rewrite_buf = NULL;
		info->v1_rewrite_len = 0;
	}
}

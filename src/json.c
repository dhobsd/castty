#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "utf8.h"

char *
json_escape(const char *from)
{
	unsigned char c;
	size_t i, o, len;
	char *p;

	assert(from);

	len = strlen(from);
	p = malloc(len * 6 + 1);
	if (p == NULL) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}

	for (i = 0, o = 0; i < len; i++) {
		c = (unsigned char)from[i];
		switch (c) {
		case '\\':
		case '"':
			p[o++] = '\\';
			p[o++] = (char)c;
			break;
		case '\b':
			p[o++] = '\\';
			p[o++] = 'b';
			break;
		case '\f':
			p[o++] = '\\';
			p[o++] = 'f';
			break;
		case '\n':
			p[o++] = '\\';
			p[o++] = 'n';
			break;
		case '\r':
			p[o++] = '\\';
			p[o++] = 'r';
			break;
		case '\t':
			p[o++] = '\\';
			p[o++] = 't';
			break;
		default:
			if (c < 0x20 || c == 0x7f) {
				o += (size_t)sprintf(&p[o], "\\u%04x", c);
			} else {
				p[o++] = (char)c;
			}
			break;
		}
	}

	p[o] = '\0';

	return p;
}

void
json_write_escaped(FILE *fp, const char *s, size_t len)
{
	uint32_t state = UTF8_ACCEPT, cp = 0;

	fputc('"', fp);
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];

		u8_decode(&state, &cp, c);
		if (state == UTF8_REJECT) {
			fputs("\\ufffd", fp);
			state = UTF8_ACCEPT;
			cp = 0;
			continue;
		}
		if (state != UTF8_ACCEPT) {
			continue;
		}

		switch (cp) {
		case '"':
		case '\\':
			fputc('\\', fp);
			fputc((int)cp, fp);
			break;
		case '\b':
			fputs("\\b", fp);
			break;
		case '\f':
			fputs("\\f", fp);
			break;
		case '\n':
			fputs("\\n", fp);
			break;
		case '\r':
			fputs("\\r", fp);
			break;
		case '\t':
			fputs("\\t", fp);
			break;
		default:
			if (cp < 0x20 || cp == 0x7f) {
				fprintf(fp, "\\u%04x", (unsigned)cp);
			} else {
				char u8[4];
				size_t n = u8_encode(cp, u8);

				if (n > 0) {
					fwrite(u8, 1, n, fp);
				}
			}
			break;
		}
	}
	if (state != UTF8_ACCEPT) {
		fputs("\\ufffd", fp);
	}
	fputc('"', fp);
}

const char *
json_skip_ws(const char *p, const char *end)
{
	while (p < end &&
	    (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
		p++;
	}
	return p;
}

const char *
json_skip_string(const char *p, const char *end)
{
	p = json_skip_ws(p, end);
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

const char *
json_parse_double(const char *p, const char *end, double *val)
{
	char buf[64], *eptr;
	size_t n = 0;

	p = json_skip_ws(p, end);
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

const char *
json_parse_int(const char *p, const char *end, int *val)
{
	double d;
	const char *next = json_parse_double(p, end, &d);

	if (next == NULL || d < (double)INT_MIN || d > (double)INT_MAX) {
		return NULL;
	}
	*val = (int)d;
	return next;
}

const char *
json_skip_array(const char *p, const char *end)
{
	p = json_skip_ws(p, end);
	if (p >= end || *p != '[') {
		return NULL;
	}
	p = json_skip_ws(p + 1, end);
	if (p < end && *p == ']') {
		return p + 1;
	}
	for (;;) {
		p = json_skip_value(p, end);
		if (p == NULL) {
			return NULL;
		}
		p = json_skip_ws(p, end);
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

const char *
json_skip_object(const char *p, const char *end)
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
		p = json_skip_string(p, end);
		if (p == NULL) {
			return NULL;
		}
		p = json_skip_ws(p, end);
		if (p >= end || *p != ':') {
			return NULL;
		}
		p = json_skip_value(p + 1, end);
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

const char *
json_skip_value(const char *p, const char *end)
{
	p = json_skip_ws(p, end);
	if (p >= end) {
		return NULL;
	}
	if (*p == '"') {
		return json_skip_string(p, end);
	}
	if (*p == '{') {
		return json_skip_object(p, end);
	}
	if (*p == '[') {
		return json_skip_array(p, end);
	}
	if (*p == '-' || isdigit((unsigned char)*p)) {
		double d;

		return json_parse_double(p, end, &d);
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
parse_hex4(const char *p, const char *end, uint32_t *out)
{
	uint32_t v = 0;

	if (end - p < 4) {
		return -1;
	}
	for (int i = 0; i < 4; i++) {
		unsigned char c = (unsigned char)p[i];

		v <<= 4;
		if (c >= '0' && c <= '9') {
			v |= (uint32_t)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			v |= (uint32_t)(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			v |= (uint32_t)(c - 'A' + 10);
		} else {
			return -1;
		}
	}
	*out = v;
	return 0;
}

const char *
json_parse_string(const char *p, const char *end, char **out_str,
    size_t *out_len)
{
	size_t cap = 32, len = 0;
	char *buf;

	p = json_skip_ws(p, end);
	if (p >= end || *p != '"') {
		return NULL;
	}
	p++;
	buf = malloc(cap);
	if (buf == NULL) {
		return NULL;
	}

	while (p < end) {
		unsigned char c = (unsigned char)*p++;
		char tmp[4];
		size_t add = 0;

		if (c == '"') {
			buf[len] = '\0';
			if (out_str != NULL) {
				*out_str = buf;
			} else {
				free(buf);
			}
			if (out_len != NULL) {
				*out_len = len;
			}
			return p;
		}
		if (c == '\\') {
			unsigned char esc;

			if (p >= end) {
				free(buf);
				return NULL;
			}
			esc = (unsigned char)*p++;
			switch (esc) {
			case '"':
			case '\\':
			case '/':
				tmp[0] = (char)esc;
				add = 1;
				break;
			case 'b':
				tmp[0] = '\b';
				add = 1;
				break;
			case 'f':
				tmp[0] = '\f';
				add = 1;
				break;
			case 'n':
				tmp[0] = '\n';
				add = 1;
				break;
			case 'r':
				tmp[0] = '\r';
				add = 1;
				break;
			case 't':
				tmp[0] = '\t';
				add = 1;
				break;
			case 'u': {
				uint32_t cp = 0;

				if (parse_hex4(p, end, &cp) != 0) {
					free(buf);
					return NULL;
				}
				p += 4;
				if (cp >= 0xd800 && cp <= 0xdbff &&
				    end - p >= 6 && p[0] == '\\' &&
				    p[1] == 'u') {
					uint32_t lo = 0;

					if (parse_hex4(p + 2, end, &lo) == 0 &&
					    lo >= 0xdc00 && lo <= 0xdfff) {
						cp = 0x10000 +
						    ((cp - 0xd800) << 10) +
						    (lo - 0xdc00);
						p += 6;
					}
				}
				add = u8_encode(cp, tmp);
				if (add == 0) {
					free(buf);
					return NULL;
				}
				break;
			}
			default:
				tmp[0] = (char)esc;
				add = 1;
				break;
			}
		} else {
			tmp[0] = (char)c;
			add = 1;
		}

		if (len + add + 1 > cap) {
			size_t ncap = cap * 2;
			char *nb = realloc(buf, ncap);

			if (nb == NULL) {
				free(buf);
				return NULL;
			}
			buf = nb;
			cap = ncap;
		}
		memcpy(buf + len, tmp, add);
		len += add;
	}

	free(buf);
	return NULL;
}

const char *
json_parse_string_buf(const char *p, const char *end, char *out, size_t cap)
{
	char *s = NULL;
	size_t slen = 0;
	const char *next = json_parse_string(p, end, &s, &slen);

	if (next == NULL) {
		return NULL;
	}
	if (cap > 0) {
		size_t copy = (slen < cap - 1) ? slen : (cap - 1);

		if (copy > 0) {
			memcpy(out, s, copy);
		}
		out[copy] = '\0';
	}
	free(s);
	return next;
}

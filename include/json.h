#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdio.h>

char *json_escape(const char *from);
void json_write_escaped(FILE *fp, const char *s, size_t len);

const char *json_skip_ws(const char *p, const char *end);
const char *json_skip_string(const char *p, const char *end);
const char *json_skip_array(const char *p, const char *end);
const char *json_skip_object(const char *p, const char *end);
const char *json_skip_value(const char *p, const char *end);

const char *json_parse_double(const char *p, const char *end, double *val);
const char *json_parse_int(const char *p, const char *end, int *val);
const char *json_parse_string(const char *p, const char *end, char **out_str,
    size_t *out_len);
const char *json_parse_string_buf(const char *p, const char *end, char *out,
    size_t cap);

#endif

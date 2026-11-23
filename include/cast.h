#ifndef CAST_H
#define CAST_H

#include <stddef.h>

struct cast_append_info {
	int exists;
	int version;
	int cols;
	int rows;
	double dur_ms;
	double v1_events_ms;
	int v1_empty;
	int need_newline;
	int existing_markers;
	long long v1_truncate_off;
	char *v1_rewrite_buf;
	size_t v1_rewrite_len;
};

int cast_inspect(const char *path, int req_version,
    struct cast_append_info *info, char *errbuf, size_t errsz);
void cast_append_info_free(struct cast_append_info *info);

#endif /* CAST_H */

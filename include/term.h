#ifndef TERM_H
#define TERM_H

struct term_probe {
	char *version;    /* JSON-escaped XTVERSION string, or NULL */
	char *theme_json; /* Serialized asciicast theme JSON object, or NULL */
};

void term_probe_tty(int infd, int outfd, const char *term_type,
    struct term_probe *out);
void term_probe_free(struct term_probe *probe);

void term_reset_init(const char *term_type, int outfd);
void term_write_reset(int outfd);

#endif

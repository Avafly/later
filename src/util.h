#ifndef LATER_UTIL_H
#define LATER_UTIL_H

#include <stddef.h>

/* "<sec>_<pid>_<rand4hex>"; buf must be >= 64 bytes. */
void id_generate(char *buf, size_t n);

/*
 * Resolve user input to a real task id.
 *   "1", "2"... — 1-based index into the sorted list
 *   "1770976574_12345_a3f1" or any unique prefix
 *
 * Returns 0 ok, -1 not found, -2 ambiguous.
 */
int id_resolve(const char *input, char *out, size_t n);

typedef struct
{
    char **items;
    size_t len;
    size_t cap;
} strvec;

void strvec_init(strvec *v);
void strvec_push(strvec *v, char *s); /* takes ownership */
void strvec_free(strvec *v);

/*
 * Read commands from stdin: tty → linenoise prompt, pipe → one line each.
 * Lines containing embedded '\n' or '\r' are rejected with a stderr warning.
 * Returns 0 on success (may be empty).
 */
int read_commands(strvec *out);

/* `dst = a "/" b`; returns 0 ok, -1 on truncation. */
int path_join(char *dst, size_t n, const char *a, const char *b);

/* recursive directory removal; equivalent to `rm -rf path`. */
int rm_rf(const char *path);

#endif

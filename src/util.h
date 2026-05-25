#ifndef LATER_UTIL_H_
#define LATER_UTIL_H_

#include "strvec.h"

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

/*
 * Read commands from stdin: tty → linenoise prompt, pipe → one line each.
 * Trailing CR is stripped. Allocates *out (which must be NULL on entry).
 * Returns 0 on success (may be empty), -1 on OOM. Caller always strvec_frees.
 */
int read_commands(strvec **out);

/* `dst = a "/" b`; returns 0 ok, -1 on truncation. */
int path_join(char *dst, size_t n, const char *a, const char *b);

/* recursive directory removal; equivalent to `rm -rf path`. */
int rm_rf(const char *path);

#endif // LATER_UTIL_H_

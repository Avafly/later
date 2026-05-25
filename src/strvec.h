#ifndef LATER_STRVEC_H_
#define LATER_STRVEC_H_

#include <stddef.h>

#define STRVEC_MAX_LEN 4096

typedef struct
{
    char **items;
    size_t len;
    size_t cap;
} strvec;

/* Allocate a fresh empty strvec at *v. *v must be NULL on entry.
 * Returns 0 on success, -1 on OOM (*v remains NULL). */
int strvec_init(strvec **v);

/* Append a copy of s to v. Returns 0 on success, -1 if the push failed
 * (allocation failed or the vector has hit STRVEC_MAX_LEN). Requires
 * v != NULL. The vector is left in a consistent state either way. */
int strvec_push(strvec *v, const char *s);

/* Release contents and the struct itself; sets *v = NULL.
 * Safe on a NULL handle. Idempotent. */
void strvec_free(strvec **v);

#endif // LATER_STRVEC_H_

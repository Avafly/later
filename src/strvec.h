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

/* Allocate an empty strvec at *v. *v must be NULL on entry.
 * Return 0 on success, -1 on failure. */
int strvec_init(strvec **v);

/* Append a copy of s to v.
 * Return 0 on success, -1 on falure. */
int strvec_push(strvec *v, const char *s);

/* Release contents and the struct itself; sets *v = NULL. */
void strvec_free(strvec **v);

#endif // LATER_STRVEC_H_

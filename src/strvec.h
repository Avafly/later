#ifndef LATER_STRVEC_H_
#define LATER_STRVEC_H_

#include <stddef.h>

/*
 * A growable list of malloc'd C strings. The list takes ownership of every
 * string pushed into it and frees them in strvec_free. The same shape is
 * used everywhere the project needs "a dynamic list of strings" (task ids,
 * commands, completion candidates, etc.) so all four ad-hoc growers
 * collapse into one type.
 */
typedef struct
{
    char **items;
    size_t len;
    size_t cap;
} strvec;

void strvec_init(strvec *v);
void strvec_push(strvec *v, char *s); /* takes ownership; OOM silently drops */
void strvec_free(strvec *v);

#endif // LATER_STRVEC_H_

#include "strvec.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int strvec_init(strvec **v)
{
    assert(v != NULL);
    assert(*v == NULL);
    *v = calloc(1, sizeof(strvec));
    return *v ? 0 : -1;
}

int strvec_push(strvec *v, const char *s)
{
    assert(v != NULL);
    if (v->len >= STRVEC_MAX_LEN)
        return -1;

    if (v->len == v->cap)
    {
        size_t nc = v->cap ? v->cap * 2 : 8;
        if (nc > STRVEC_MAX_LEN)
            nc = STRVEC_MAX_LEN;
        char **ni = realloc(v->items, nc * sizeof(*ni));
        if (!ni)
            return -1;
        v->items = ni;
        v->cap = nc;
    }

    char *copy = strdup(s);
    if (!copy)
        return -1;
    v->items[v->len++] = copy;
    return 0;
}

void strvec_free(strvec **v)
{
    if (!v || !*v)
        return;
    for (size_t i = 0; i < (*v)->len; ++i)
        free((*v)->items[i]);
    free((*v)->items);
    free(*v);
    *v = NULL;
}

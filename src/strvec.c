#include "strvec.h"

#include <stdlib.h>

void strvec_init(strvec *v)
{
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

void strvec_push(strvec *v, char *s)
{
    if (v->len == v->cap)
    {
        size_t nc = v->cap ? v->cap * 2 : 8;
        char **ni = realloc(v->items, nc * sizeof(*ni));
        if (!ni)
        {
            free(s);
            return;
        }
        v->items = ni;
        v->cap = nc;
    }
    v->items[v->len++] = s;
}

void strvec_free(strvec *v)
{
    if (!v)
        return;
    for (size_t i = 0; i < v->len; ++i)
        free(v->items[i]);
    free(v->items);
    v->items = NULL;
    v->len = v->cap = 0;
}

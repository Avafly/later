#include "util.h"

#include "store.h"
#include "3rdparty/linenoise/linenoise.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

void id_generate(char *buf, size_t n)
{
    unsigned r = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &r, sizeof(r)) != (ssize_t)sizeof(r)) {
            /* fall back to time-based scramble if urandom fails */
            r = (unsigned)time(NULL) ^ (unsigned)getpid();
        }
        close(fd);
    } else {
        r = (unsigned)time(NULL) ^ (unsigned)getpid();
    }
    snprintf(buf, n, "%lld_%d_%04x",
             (long long)time(NULL), (int)getpid(), r & 0xFFFFu);
}

int id_resolve(const char *input, char *out, size_t n)
{
    task_id_list_t list = {0};
    if (store_list(&list) < 0)
        return -1;

    /* try as 1-based index */
    int all_digits = input[0] != '\0';
    for (const char *p = input; *p; ++p) {
        if (!isdigit((unsigned char)*p)) { all_digits = 0; break; }
    }
    if (all_digits) {
        char *end;
        long idx = strtol(input, &end, 10);
        if (*end == '\0' && idx >= 1 && (size_t)idx <= list.len) {
            snprintf(out, n, "%s", list.ids[idx - 1]);
            store_list_free(&list);
            return 0;
        }
    }

    /* exact match first */
    for (size_t i = 0; i < list.len; ++i) {
        if (strcmp(list.ids[i], input) == 0) {
            snprintf(out, n, "%s", list.ids[i]);
            store_list_free(&list);
            return 0;
        }
    }

    /* unique prefix */
    size_t in_len = strlen(input);
    int matches = 0;
    const char *hit = NULL;
    for (size_t i = 0; i < list.len; ++i) {
        if (strncmp(list.ids[i], input, in_len) == 0) {
            matches++;
            hit = list.ids[i];
        }
    }
    int rc;
    if (matches == 1) {
        snprintf(out, n, "%s", hit);
        rc = 0;
    } else {
        rc = (matches == 0) ? -1 : -2;
    }
    store_list_free(&list);
    return rc;
}

/* --- strvec --------------------------------------------------------------- */

void strvec_init(strvec_t *v)
{
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

void strvec_push(strvec_t *v, char *s)
{
    if (v->len == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 8;
        char **ni = realloc(v->items, nc * sizeof(*ni));
        if (!ni) { free(s); return; }
        v->items = ni;
        v->cap = nc;
    }
    v->items[v->len++] = s;
}

void strvec_free(strvec_t *v)
{
    if (!v) return;
    for (size_t i = 0; i < v->len; ++i) free(v->items[i]);
    free(v->items);
    v->items = NULL;
    v->len = v->cap = 0;
}

/* --- read_commands -------------------------------------------------------- */

/* Path completion for the linenoise prompt. Token is the trailing whitespace-
 * separated word; we offer entries from its directory part. Spaces are escaped
 * with '\ ' both ways. */
static void unescape_path(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < n; ++i) {
        if (in[i] == '\\' && in[i + 1] == ' ') {
            out[o++] = ' ';
            ++i;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

static void escape_and_append(const char *in, char *out, size_t n)
{
    size_t o = strlen(out);
    for (size_t i = 0; in[i] && o + 2 < n; ++i) {
        if (in[i] == ' ') {
            if (o + 3 >= n) break;
            out[o++] = '\\';
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

static int cmp_strp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void completion_cb(const char *buf, linenoiseCompletions *lc)
{
    /* find start of current token (the last unescaped space) */
    size_t len = strlen(buf);
    size_t token_start = 0;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == ' ' && (i == 0 || buf[i - 1] != '\\'))
            token_start = i + 1;
    }

    char context[1024];
    snprintf(context, sizeof(context), "%.*s", (int)token_start, buf);

    char token[1024];
    snprintf(token, sizeof(token), "%s", buf + token_start);

    char raw[1024];
    unescape_path(token, raw, sizeof(raw));

    char dir[1024];
    const char *prefix = raw;
    const char *slash = strrchr(raw, '/');
    if (slash) {
        size_t dlen = (slash == raw) ? 1 : (size_t)(slash - raw);
        if (dlen >= sizeof(dir)) return;
        memcpy(dir, raw, dlen); dir[dlen] = '\0';
        prefix = slash + 1;
    } else if (strcmp(raw, "~") == 0) {
        snprintf(dir, sizeof(dir), "~");
        prefix = "";
    } else {
        snprintf(dir, sizeof(dir), ".");
    }

    char search_dir[PATH_MAX];
    if (dir[0] == '~' && (dir[1] == '\0' || dir[1] == '/')) {
        const char *home = getenv("HOME");
        if (!home) return;
        snprintf(search_dir, sizeof(search_dir), "%s%s", home, dir + 1);
    } else {
        snprintf(search_dir, sizeof(search_dir), "%s", dir);
    }

    DIR *d = opendir(search_dir);
    if (!d) return;

    char **matches = NULL;
    size_t mlen = 0, mcap = 0;
    size_t prefix_len = strlen(prefix);

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && prefix[0] != '.') continue;
        if (strncmp(e->d_name, prefix, prefix_len) != 0) continue;

        char entry[PATH_MAX];
        if ((size_t)snprintf(entry, sizeof(entry), "%s/%s",
                             search_dir, e->d_name) >= sizeof(entry))
            continue;
        struct stat st;
        int is_dir = (stat(entry, &st) == 0 && S_ISDIR(st.st_mode));

        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s%s", e->d_name, is_dir ? "/" : "");

        if (mlen == mcap) {
            size_t nc = mcap ? mcap * 2 : 8;
            char **nm = realloc(matches, nc * sizeof(*nm));
            if (!nm) break;
            matches = nm; mcap = nc;
        }
        matches[mlen] = strdup(tmp);
        if (!matches[mlen]) break;
        mlen++;
    }
    closedir(d);

    qsort(matches, mlen, sizeof(*matches), cmp_strp);

    for (size_t i = 0; i < mlen; ++i) {
        char completion[PATH_MAX];
        snprintf(completion, sizeof(completion), "%s", context);
        if (slash) {
            size_t off = strlen(completion);
            size_t keep = (size_t)(strrchr(token, '/') - token + 1);
            if (off + keep < sizeof(completion)) {
                memcpy(completion + off, token, keep);
                completion[off + keep] = '\0';
            }
        } else if (strcmp(raw, "~") == 0) {
            strncat(completion, "~/",
                    sizeof(completion) - strlen(completion) - 1);
        }
        escape_and_append(matches[i], completion, sizeof(completion));
        if (mlen == 1) {
            size_t cl = strlen(completion);
            if (cl > 0 && completion[cl - 1] != '/'
                && cl + 1 < sizeof(completion)) {
                completion[cl] = ' ';
                completion[cl + 1] = '\0';
            }
        }
        linenoiseAddCompletion(lc, completion);
        free(matches[i]);
    }
    free(matches);
}

static int read_commands_tty(strvec_t *out)
{
    linenoiseSetCompletionCallback(completion_cb);
    while (1) {
        char *line = linenoise("later> ");
        if (!line) break;
        if (line[0] == '\0') { linenoiseFree(line); break; }
        linenoiseHistoryAdd(line);
        strvec_push(out, strdup(line));
        linenoiseFree(line);
    }
    return 0;
}

static int read_commands_pipe(strvec_t *out)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t got;
    while ((got = getline(&line, &cap, stdin)) > 0) {
        if (line[got - 1] == '\n') line[--got] = '\0';
        if (got > 0 && line[got - 1] == '\r') line[--got] = '\0';
        if (got == 0) continue;
        char *dup = strdup(line);
        if (!dup) { free(line); return -1; }
        strvec_push(out, dup);
    }
    free(line);
    return 0;
}

int read_commands(strvec_t *out)
{
    if (isatty(STDIN_FILENO))
        return read_commands_tty(out);
    return read_commands_pipe(out);
}

int path_join(char *dst, size_t n, const char *a, const char *b)
{
    int r = snprintf(dst, n, "%s/%s", a, b);
    return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

int rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return (errno == ENOENT) ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);

    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char sub[PATH_MAX];
        if ((size_t)snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name)
            >= sizeof(sub)) { rc = -1; break; }
        if (rm_rf(sub) < 0) { rc = -1; break; }
    }
    closedir(d);
    if (rc == 0) rc = rmdir(path);
    return rc;
}

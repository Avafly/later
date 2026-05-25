#include "store.h"

#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char g_base_dir[PATH_MAX];
static int g_base_inited;

static int mkdirs(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, n + 1);

    for (size_t i = 1; i < n; ++i)
    {
        if (tmp[i] == '/')
        {
            tmp[i] = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST)
                return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int init_base_dir(void)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0])
    {
        if ((size_t)snprintf(g_base_dir, sizeof(g_base_dir), "%s/later", xdg) >= sizeof(g_base_dir))
            return -1;
    }
    else
    {
        const char *home = getenv("HOME");
        if (!home || !home[0])
        {
            fprintf(stderr, "Error: HOME is not set\n");
            return -1;
        }
        if ((size_t)snprintf(g_base_dir, sizeof(g_base_dir), "%s/.local/share/later", home) >=
            sizeof(g_base_dir))
            return -1;
    }
    g_base_inited = 1;
    return 0;
}

int store_ensure_base(void)
{
    if (!g_base_inited && init_base_dir() < 0)
        return -1;
    return mkdirs(g_base_dir, 0755);
}

const char *store_base_dir(void)
{
    if (!g_base_inited)
        init_base_dir();
    return g_base_dir;
}

int store_task_dir(const char *id, char *buf, size_t n)
{
    if (!g_base_inited && init_base_dir() < 0)
        return -1;
    int r = snprintf(buf, n, "%s/%s", g_base_dir, id);
    return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

int store_path_in_task(const char *id, const char *name, char *buf, size_t n)
{
    if (!g_base_inited && init_base_dir() < 0)
        return -1;
    int r = snprintf(buf, n, "%s/%s/%s", g_base_dir, id, name);
    return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

/* --- marker primitives ---------------------------------------------------- */

int store_create_marker(const char *id, const char *name)
{
    char p[PATH_MAX];
    if (store_path_in_task(id, name, p, sizeof(p)) < 0)
        return -1;
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return (errno == EEXIST) ? 0 : -1;
    close(fd);
    return 0;
}

int store_create_marker_with_content(const char *id, const char *name, const char *content)
{
    char p[PATH_MAX];
    if (store_path_in_task(id, name, p, sizeof(p)) < 0)
        return -1;
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return (errno == EEXIST) ? 0 : -1;

    size_t len = strlen(content);
    size_t off = 0;
    while (off < len)
    {
        ssize_t w = write(fd, content + off, len - off);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }
    if (fsync(fd) < 0)
    {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int store_marker_exists(const char *id, const char *name)
{
    char p[PATH_MAX];
    if (store_path_in_task(id, name, p, sizeof(p)) < 0)
        return 0;
    struct stat st;
    return (stat(p, &st) == 0) ? 1 : 0;
}

ssize_t store_marker_read(const char *id, const char *name, char *buf, size_t n)
{
    char p[PATH_MAX];
    if (store_path_in_task(id, name, p, sizeof(p)) < 0)
        return -1;
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return (errno == ENOENT) ? 0 : -1;

    if (n == 0)
    {
        close(fd);
        return -1;
    }

    size_t total = 0;
    while (total < n - 1)
    {
        ssize_t r = read(fd, buf + total, n - 1 - total);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    close(fd);
    return (ssize_t)total;
}

int store_marker_remove(const char *id, const char *name)
{
    char p[PATH_MAX];
    if (store_path_in_task(id, name, p, sizeof(p)) < 0)
        return -1;
    if (unlink(p) < 0 && errno != ENOENT)
        return -1;
    return 0;
}

int store_fsync_marker(const char *id, const char *name)
{
    char p[PATH_MAX];
    if (store_path_in_task(id, name, p, sizeof(p)) < 0)
        return -1;
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

/* --- lock ----------------------------------------------------------------- */

int store_acquire_lock(const char *id)
{
    char p[PATH_MAX];
    if (store_path_in_task(id, "lock", p, sizeof(p)) < 0)
        return -1;
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

int store_is_locked(const char *id)
{
    char p[PATH_MAX];
    if (store_path_in_task(id, "lock", p, sizeof(p)) < 0)
        return 0;
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    if (flock(fd, LOCK_SH | LOCK_NB) < 0)
    {
        int locked = (errno == EWOULDBLOCK || errno == EAGAIN);
        close(fd);
        return locked;
    }
    flock(fd, LOCK_UN);
    close(fd);
    return 0;
}

/* --- meta ----------------------------------------------------------------- */

int store_write_meta(const task_meta *m)
{
    char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
    if (store_task_dir(m->id, dir, sizeof(dir)) < 0)
        return -1;
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
        return -1;
    if (store_path_in_task(m->id, "meta", path, sizeof(path)) < 0)
        return -1;
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid()) >= sizeof(tmp))
        return -1;

    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "id=%s\n", m->id);
    fprintf(f, "cwd=%s\n", m->cwd);
    fprintf(f, "created_at=%lld\n", (long long)m->created_at);
    fprintf(f, "execute_at=%lld\n", (long long)m->execute_at);
    fprintf(f, "daemon_pid=%lld\n", (long long)m->daemon_pid);
    int werr = ferror(f);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0)
        werr = 1;
    if (fclose(f) != 0)
        werr = 1;
    if (werr)
    {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) < 0)
    {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int parse_kv_line(char *line, char **k, char **v)
{
    char *eq = strchr(line, '=');
    if (!eq)
        return -1;
    *eq = '\0';
    *k = line;
    *v = eq + 1;
    size_t vn = strlen(*v);
    if (vn && (*v)[vn - 1] == '\n')
        (*v)[vn - 1] = '\0';
    return 0;
}

int store_read_meta(const char *id, task_meta *out)
{
    char path[PATH_MAX];
    if (store_path_in_task(id, "meta", path, sizeof(path)) < 0)
        return -1;
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    memset(out, 0, sizeof(*out));
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof(line), f))
    {
        char *k, *v;
        if (parse_kv_line(line, &k, &v) < 0)
            continue;
        if (strcmp(k, "id") == 0)
            snprintf(out->id, sizeof(out->id), "%s", v);
        else if (strcmp(k, "cwd") == 0)
            snprintf(out->cwd, sizeof(out->cwd), "%s", v);
        else if (strcmp(k, "created_at") == 0)
            out->created_at = (time_t)strtoll(v, NULL, 10);
        else if (strcmp(k, "execute_at") == 0)
            out->execute_at = (time_t)strtoll(v, NULL, 10);
        else if (strcmp(k, "daemon_pid") == 0)
            out->daemon_pid = (pid_t)strtoll(v, NULL, 10);
    }
    int err = ferror(f);
    fclose(f);
    return err ? -1 : 0;
}

/* --- commands ------------------------------------------------------------- */

int store_write_commands(const char *id, char *const *cmds, size_t n)
{
    char path[PATH_MAX], tmp[PATH_MAX];
    if (store_path_in_task(id, "commands", path, sizeof(path)) < 0)
        return -1;
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid()) >= sizeof(tmp))
        return -1;

    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    for (size_t i = 0; i < n; ++i)
    {
        for (const char *p = cmds[i]; *p; ++p)
        {
            if (*p == '\n' || *p == '\r')
            {
                fclose(f);
                unlink(tmp);
                errno = EINVAL;
                return -1;
            }
        }
        fprintf(f, "%s\n", cmds[i]);
    }
    int werr = ferror(f);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0)
        werr = 1;
    if (fclose(f) != 0)
        werr = 1;
    if (werr)
    {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) < 0)
    {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int store_read_commands(const char *id, strvec *out)
{
    strvec_init(out);

    char path[PATH_MAX];
    if (store_path_in_task(id, "commands", path, sizeof(path)) < 0)
        return -1;
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char *line = NULL;
    size_t cap_line = 0;
    ssize_t got;
    while ((got = getline(&line, &cap_line, f)) > 0)
    {
        if (line[got - 1] == '\n')
            line[got - 1] = '\0';
        char *dup = strdup(line);
        if (!dup)
        {
            free(line);
            fclose(f);
            strvec_free(out);
            return -1;
        }
        strvec_push(out, dup);
    }
    free(line);
    int err = ferror(f);
    fclose(f);
    if (err)
    {
        strvec_free(out);
        return -1;
    }
    return 0;
}

/* --- list ----------------------------------------------------------------- */

static int cmp_by_created_at(const void *a, const void *b)
{
    const char *ia = *(const char *const *)a;
    const char *ib = *(const char *const *)b;
    task_meta ma, mb;
    int ra = store_read_meta(ia, &ma);
    int rb = store_read_meta(ib, &mb);
    /* unreadable meta sorts last so it doesn't crash the table */
    if (ra < 0 && rb < 0)
        return strcmp(ia, ib);
    if (ra < 0)
        return 1;
    if (rb < 0)
        return -1;
    if (ma.created_at < mb.created_at)
        return -1;
    if (ma.created_at > mb.created_at)
        return 1;
    return strcmp(ia, ib);
}

int store_list(strvec *out)
{
    strvec_init(out);
    if (!g_base_inited && init_base_dir() < 0)
        return -1;

    DIR *d = opendir(g_base_dir);
    if (!d)
        return (errno == ENOENT) ? 0 : -1;

    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.')
            continue;
        char p[PATH_MAX];
        if (store_path_in_task(e->d_name, "meta", p, sizeof(p)) < 0)
            continue;
        struct stat st;
        if (stat(p, &st) < 0)
            continue; /* skip incomplete task dirs */

        char *dup = strdup(e->d_name);
        if (!dup)
        {
            closedir(d);
            strvec_free(out);
            return -1;
        }
        strvec_push(out, dup);
    }
    closedir(d);

    qsort(out->items, out->len, sizeof(*out->items), cmp_by_created_at);
    return 0;
}

/* --- status resolution: the heart of the state model ---------------------- */

task_status store_resolve_status(const char *id)
{
    /* External markers come first: pause/cancel describe intent that
     * overrides what the daemon is doing. */
    int paused = store_marker_exists(id, "pause");
    int cancelled = store_marker_exists(id, "cancel");
    int done = store_marker_exists(id, "done");
    int errf = store_marker_exists(id, "error");
    int running = store_marker_exists(id, "running");
    int locked = store_is_locked(id);

    if (paused && locked)
        return STATUS_PAUSED;
    if (cancelled)
        return STATUS_CANCELLED;
    if (done)
        return STATUS_COMPLETED;
    if (errf)
        return STATUS_FAILED;
    if (running)
        return locked ? STATUS_RUNNING : STATUS_FAILED;
    /* no markers: still waiting or crashed before execution */
    return locked ? STATUS_PENDING : STATUS_FAILED;
}

/* --- delete --------------------------------------------------------------- */

int store_delete(const char *id)
{
    char dir[PATH_MAX];
    if (store_task_dir(id, dir, sizeof(dir)) < 0)
        return -1;
    return rm_rf(dir);
}

/* --- status names --------------------------------------------------------- */

const char *status_name(task_status s)
{
    switch (s)
    {
        case STATUS_PENDING:
            return "pending";
        case STATUS_RUNNING:
            return "running";
        case STATUS_COMPLETED:
            return "completed";
        case STATUS_FAILED:
            return "failed";
        case STATUS_CANCELLED:
            return "cancelled";
        case STATUS_PAUSED:
            return "paused";
    }
    return "unknown";
}

const char *status_name_color(task_status s)
{
    switch (s)
    {
        case STATUS_PENDING:
            return "\033[33mpending\033[0m";
        case STATUS_RUNNING:
            return "\033[34mrunning\033[0m";
        case STATUS_COMPLETED:
            return "\033[32mcompleted\033[0m";
        case STATUS_FAILED:
            return "\033[31mfailed\033[0m";
        case STATUS_CANCELLED:
            return "\033[90mcancelled\033[0m";
        case STATUS_PAUSED:
            return "\033[36mpaused\033[0m";
    }
    return "unknown";
}

int status_is_final(task_status s)
{
    return s == STATUS_COMPLETED || s == STATUS_FAILED || s == STATUS_CANCELLED;
}

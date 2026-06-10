#include "store.h"

#include "strvec.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static char g_base_dir[PATH_MAX];

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
            if (ensure_dir(tmp, mode) < 0)
                return -1;
            tmp[i] = '/';
        }
    }
    return ensure_dir(tmp, mode);
}

static int init_base_dir(void)
{
    const char *xdg_path = getenv("XDG_DATA_HOME");
    if (xdg_path && xdg_path[0])
    {
        if ((size_t)snprintf(g_base_dir, sizeof(g_base_dir), "%s/later", xdg_path) >=
            sizeof(g_base_dir))
            return -1;
    }
    else
    {
        const char *home_path = getenv("HOME");
        if (!home_path || !home_path[0])
        {
            fprintf(stderr, "Error: HOME is not set\n");
            return -1;
        }
        if ((size_t)snprintf(g_base_dir, sizeof(g_base_dir), "%s/.local/share/later", home_path) >=
            sizeof(g_base_dir))
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

static int fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

static int cmp_by_created_at(const void *a, const void *b)
{
    const char *ia = *(const char *const *)a;
    const char *ib = *(const char *const *)b;
    task_meta ma, mb;
    int ra = store_read_meta(ia, &ma);
    int rb = store_read_meta(ib, &mb);
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

static int color_enabled(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = isatty(STDOUT_FILENO) ? 1 : 0;
    return cached;
}

static void reap_orphan_group(const char *id)
{
    task_meta meta;
    if (store_read_meta(id, &meta) < 0 || meta.daemon_pid <= 1)
        return;
    int leader_alive = (kill(meta.daemon_pid, 0) == 0);
    int group_alive = (kill(-meta.daemon_pid, 0) == 0);
    if (group_alive && !leader_alive)
        kill(-meta.daemon_pid, SIGKILL);
}

int store_ensure_base(void)
{
    if (g_base_dir[0] == '\0' && init_base_dir() < 0)
        return -1;
    return mkdirs(g_base_dir, 0755);
}

const char *store_base_dir(void)
{
    if (g_base_dir[0] == '\0')
        init_base_dir();
    return g_base_dir;
}

int store_task_dir(const char *id, char *buf, size_t n)
{
    if (g_base_dir[0] == '\0' && init_base_dir() < 0)
        return -1;
    return ((size_t)snprintf(buf, n, "%s/%s", g_base_dir, id) >= n) ? -1 : 0;
}

int store_path_in_task(const char *id, const char *name, char *buf, size_t n)
{
    if (g_base_dir[0] == '\0' && init_base_dir() < 0)
        return -1;
    return ((size_t)snprintf(buf, n, "%s/%s/%s", g_base_dir, id, name) >= n) ? -1 : 0;
}

int store_acquire_lock(const char *id)
{
    char path[PATH_MAX];
    if (store_path_in_task(id, "lock", path, sizeof(path)) < 0)
        return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
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
    char path[PATH_MAX];
    if (store_path_in_task(id, "lock", path, sizeof(path)) < 0)
        return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
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

int store_write_meta(const task_meta *meta)
{
    char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
    if (store_task_dir(meta->id, dir, sizeof(dir)) < 0)
        return -1;
    if (ensure_dir(dir, 0755) < 0)
        return -1;
    if (store_path_in_task(meta->id, "meta", path, sizeof(path)) < 0)
        return -1;
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid()) >= sizeof(tmp))
        return -1;

    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "id=%s\n", meta->id);
    fprintf(f, "cwd=%s\n", meta->cwd);
    fprintf(f, "created_at=%lld\n", (long long)meta->created_at);
    fprintf(f, "execute_at=%lld\n", (long long)meta->execute_at);
    fprintf(f, "daemon_pid=%lld\n", (long long)meta->daemon_pid);
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
    return fsync_dir(dir);
}

int store_read_meta(const char *id, task_meta *meta)
{
    char path[PATH_MAX];
    if (store_path_in_task(id, "meta", path, sizeof(path)) < 0)
        return -1;
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    memset(meta, 0, sizeof(*meta));
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof(line), f))
    {
        char *k, *v;
        if (parse_kv_line(line, &k, &v) < 0)
            continue;
        if (strcmp(k, "id") == 0)
            snprintf(meta->id, sizeof(meta->id), "%s", v);
        else if (strcmp(k, "cwd") == 0)
            snprintf(meta->cwd, sizeof(meta->cwd), "%s", v);
        else if (strcmp(k, "created_at") == 0)
            meta->created_at = (time_t)strtoll(v, NULL, 10);
        else if (strcmp(k, "execute_at") == 0)
            meta->execute_at = (time_t)strtoll(v, NULL, 10);
        else if (strcmp(k, "daemon_pid") == 0)
            meta->daemon_pid = (pid_t)strtoll(v, NULL, 10);
    }
    int err = ferror(f);
    fclose(f);
    return err ? -1 : 0;
}

int store_task_group_alive(const char *id)
{
    task_meta meta;
    if (store_read_meta(id, &meta) < 0 || meta.daemon_pid <= 1)
        return 0;
    if (kill(-meta.daemon_pid, 0) != 0)
        return 0; // group gone
    if (store_is_locked(id))
        return 1;
    return kill(meta.daemon_pid, 0) != 0;
}

int store_write_commands(const char *id, char *const *cmds, size_t n)
{
    char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
    if (store_task_dir(id, dir, sizeof(dir)) < 0)
        return -1;
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
    return fsync_dir(dir);
}

int store_read_commands(const char *id, strvec **cmds)
{
    if (strvec_init(cmds) < 0)
        return -1;
    strvec *v = *cmds;

    char path[PATH_MAX];
    if (store_path_in_task(id, "commands", path, sizeof(path)) < 0)
        return -1;
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    int rc = 0;
    char *line = NULL;
    size_t cap_line = 0;
    ssize_t got;
    while ((got = getline(&line, &cap_line, f)) > 0)
    {
        if (line[got - 1] == '\n')
            line[got - 1] = '\0';
        if (strvec_push(v, line) < 0)
        {
            rc = -1;
            break;
        }
    }
    free(line);
    if (ferror(f))
        rc = -1;
    fclose(f);
    return rc;
}

int store_create_marker(const char *id, const char *name)
{
    char dir[PATH_MAX], path[PATH_MAX];
    if (store_task_dir(id, dir, sizeof(dir)) < 0)
        return -1;
    if (store_path_in_task(id, name, path, sizeof(path)) < 0)
        return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return (errno == EEXIST) ? 0 : -1;
    if (fsync(fd) < 0)
    {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    return fsync_dir(dir);
}

int store_create_marker_with_content(const char *id, const char *name, const char *content)
{
    char dir[PATH_MAX], path[PATH_MAX];
    if (store_task_dir(id, dir, sizeof(dir)) < 0)
        return -1;
    if (store_path_in_task(id, name, path, sizeof(path)) < 0)
        return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return (errno == EEXIST) ? 0 : -1;

    size_t len = strlen(content);
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = write(fd, content + off, len - off);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(path);
            return -1;
        }
        off += (size_t)n;
    }
    if (fsync(fd) < 0)
    {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    return fsync_dir(dir);
}

int store_has_marker(const char *id, const char *name)
{
    char path[PATH_MAX];
    if (store_path_in_task(id, name, path, sizeof(path)) < 0)
        return 0;
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
}

ssize_t store_read_marker(const char *id, const char *name, char *buf, size_t n)
{
    char path[PATH_MAX];
    if (store_path_in_task(id, name, path, sizeof(path)) < 0)
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return (errno == ENOENT) ? 0 : -1;

    if (n == 0)
    {
        close(fd);
        return -1;
    }

    size_t off = 0;
    while (off < n - 1)
    {
        ssize_t r = read(fd, buf + off, n - 1 - off);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        off += (size_t)r;
    }
    buf[off] = '\0';
    close(fd);
    return (ssize_t)off;
}

int store_remove_marker(const char *id, const char *name)
{
    char path[PATH_MAX];
    if (store_path_in_task(id, name, path, sizeof(path)) < 0)
        return -1;
    if (unlink(path) < 0 && errno != ENOENT)
        return -1;
    return 0;
}

int store_list(strvec **list)
{
    if (strvec_init(list) < 0)
        return -1;
    strvec *v = *list;

    if (g_base_dir[0] == '\0' && init_base_dir() < 0)
        return -1;

    DIR *d = opendir(g_base_dir);
    if (!d)
        return (errno == ENOENT) ? 0 : -1;

    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (!is_task_id(e->d_name))
            continue;
        char dir[PATH_MAX];
        if (store_task_dir(e->d_name, dir, sizeof(dir)) < 0)
            continue;
        struct stat st;
        if (lstat(dir, &st) < 0 || !S_ISDIR(st.st_mode))
            continue;

        if (strvec_push(v, e->d_name) < 0)
        {
            closedir(d);
            return -1;
        }
    }
    closedir(d);

    qsort(v->items, v->len, sizeof(*v->items), cmp_by_created_at);
    return 0;
}

task_status store_resolve_status(const char *id)
{
    int paused = store_has_marker(id, "pause");
    int cancelled = store_has_marker(id, "cancel");
    int done = store_has_marker(id, "done");
    int error = store_has_marker(id, "error");
    int running = store_has_marker(id, "running");
    int locked = store_is_locked(id);

    if (done)
        return STATUS_COMPLETED;
    if (error)
        return STATUS_FAILED;
    if (cancelled)
        return STATUS_CANCELLED;
    if (paused && locked)
        return STATUS_PAUSED;
    if (running)
        return locked ? STATUS_RUNNING : STATUS_FAILED;
    return locked ? STATUS_PENDING : STATUS_FAILED;
}

const char *store_status_name(task_status st)
{
    switch (st)
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

const char *store_status_color_prefix(task_status st)
{
    if (!color_enabled())
        return "";
    switch (st)
    {
        case STATUS_PENDING:
            return "\033[33m";
        case STATUS_RUNNING:
            return "\033[34m";
        case STATUS_COMPLETED:
            return "\033[32m";
        case STATUS_FAILED:
            return "\033[31m";
        case STATUS_CANCELLED:
            return "\033[90m";
        case STATUS_PAUSED:
            return "\033[36m";
    }
    return "";
}

const char *store_status_color_suffix(void)
{
    return color_enabled() ? "\033[0m" : "";
}

int store_status_is_final(task_status st)
{
    return st == STATUS_COMPLETED || st == STATUS_FAILED || st == STATUS_CANCELLED;
}

int store_delete_task(const char *id)
{
    if (!is_task_id(id))
        return -1;
    char dir[PATH_MAX];
    if (store_task_dir(id, dir, sizeof(dir)) < 0)
        return -1;
    reap_orphan_group(id);
    return rm_rf(dir);
}

int store_list_foreign(strvec **foreign)
{
    if (strvec_init(foreign) < 0)
        return -1;
    strvec *v = *foreign;

    if (g_base_dir[0] == '\0' && init_base_dir() < 0)
        return -1;

    DIR *d = opendir(g_base_dir);
    if (!d)
        return (errno == ENOENT) ? 0 : -1;

    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (is_task_id(e->d_name))
            continue;
        if (strvec_push(v, e->d_name) < 0)
        {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

int store_remove_base(void)
{
    if (g_base_dir[0] == '\0' && init_base_dir() < 0)
        return -1;
    return rmdir(g_base_dir);
}

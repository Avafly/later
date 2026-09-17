#include "action.h"

#include "daemon.h"
#include "store.h"
#include "strvec.h"
#include "timefmt.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
    time_t at;
    char wait_for[64];
    int wait_require; /* non-zero: wait_for must complete successfully */
} trigger;

static void print_task_header(const trigger *t, time_t now, const char *cwd)
{
    if (t->wait_for[0])
    {
        if (t->wait_require)
            printf("Execute at:  requires task %s to complete\n", t->wait_for);
        else
            printf("Execute at:  after task %s ends\n", t->wait_for);
    }
    else
    {
        char scheduled[64], duration[64];
        timefmt_format_time(t->at, scheduled, sizeof(scheduled));
        timefmt_format_duration((long)(t->at - now), duration, sizeof(duration));
        printf("Execute at:  %s (%s)\n", scheduled, duration);
    }
    printf("Working dir: %s\n", cwd);
}

/* Fork the daemon and wait for its readiness signal.
 * Return 0 if the daemon reported success, or 1 on failure. */
static int spawn_task(const trigger *t, time_t now, const char *cwd, const strvec *cmds)
{
    task_meta meta = {0};
    generate_id(meta.id, sizeof(meta.id));
    snprintf(meta.cwd, sizeof(meta.cwd), "%s", cwd);
    snprintf(meta.wait_for, sizeof(meta.wait_for), "%s", t->wait_for);
    meta.wait_require = t->wait_require;
    meta.created_at = now;
    meta.execute_at = t->wait_for[0] ? now : t->at;
    meta.daemon_pid = -1;

    int pipefd[2];
    if (pipe(pipefd) < 0)
    {
        fprintf(stderr, "Error: pipe: %s\n", strerror(errno));
        return 1;
    }

    // drain stdio buffers before fork
    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "Error: fork: %s\n", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }

    // child: become the daemon
    if (pid == 0)
    {
        close(pipefd[0]);
        daemon_run(meta, cmds->items, cmds->len, pipefd[1]);
        _exit(1);
    }

    // parent: wait for the daemon to report readiness
    close(pipefd[1]);

    char report[512];
    size_t off = 0;
    while (off < sizeof(report) - 1)
    {
        ssize_t r = read(pipefd[0], report + off, sizeof(report) - 1 - off);
        if (r > 0)
        {
            off += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        break;
    }
    report[off] = '\0';
    close(pipefd[0]);

    if (off > 0 && report[0] == 'k')
    {
        printf("Task %s created\n", meta.id);
        return 0;
    }
    if (off > 0 && report[0] == 'e')
    {
        fprintf(stderr, "Error: %s\n", report + 1);
        return 1;
    }
    fprintf(stderr, "Error: daemon failed to start\n");
    return 1;
}

static int resolve_or_error(const char *input, char *out, size_t n)
{
    int rc = resolve_id(input, out, n);
    if (rc == -1)
        fprintf(stderr, "Error: task '%s' not found\n", input);
    else if (rc == -2)
        fprintf(stderr, "Error: task '%s' is ambiguous\n", input);
    return rc;
}

static int parse_trigger(const char *time_str, const char *after_id, const char *require_id,
                         trigger *out)
{
    memset(out, 0, sizeof(*out));

    int given = (time_str != NULL) + (after_id != NULL) + (require_id != NULL);
    if (given > 1)
    {
        fprintf(stderr, "Error: <time>, --after and --require are mutually exclusive\n");
        return -1;
    }
    if (given == 0)
    {
        fprintf(stderr, "Error: a time, --after or --require is required\n");
        return -1;
    }

    if (time_str)
    {
        char errbuf[256];
        if (timefmt_parse_time(time_str, &out->at, errbuf, sizeof(errbuf)) < 0)
        {
            fprintf(stderr, "Error: %s\n", errbuf);
            return -1;
        }
        return 0;
    }

    if (resolve_or_error(require_id ? require_id : after_id, out->wait_for,
                         sizeof(out->wait_for)) < 0)
        return -1;
    out->wait_require = (require_id != NULL);

    if (out->wait_require)
    {
        task_status st = store_resolve_status(out->wait_for);
        if (st != STATUS_COMPLETED && store_status_is_final(st))
        {
            fprintf(stderr, "Error: task %s is already %s\n", out->wait_for, store_status_name(st));
            return -1;
        }
    }
    return 0;
}

/* Return 1 if any task's process group still has a live member. */
static int any_group_alive(const strvec *tasks)
{
    for (size_t i = 0; i < tasks->len; ++i)
    {
        if (store_task_group_alive(tasks->items[i]))
            return 1;
    }
    return 0;
}

static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int kill_alive_groups(const strvec *tasks)
{
    int n = 0;
    for (size_t i = 0; i < tasks->len; ++i)
    {
        task_meta meta;
        if (store_task_group_alive(tasks->items[i]) &&
            store_read_meta(tasks->items[i], &meta) == 0 && meta.daemon_pid > 1)
        {
            kill(-meta.daemon_pid, SIGKILL);
            ++n;
        }
    }
    return n;
}

/* Wait up to 10s for the groups to exit, then SIGKILL any that ignored it.
 * Return how many were force-killed. */
static int wait_then_kill(const strvec *tasks)
{
    int waited = 0;
    while (waited < 10000 && any_group_alive(tasks))
    {
        sleep_ms(100);
        waited += 100;
    }
    int forced = 0;
    if (any_group_alive(tasks))
    {
        forced = kill_alive_groups(tasks);
        for (int k = 0; k < 10 && any_group_alive(tasks); ++k)
            sleep_ms(100);
    }
    return forced;
}

static size_t list_index_of(const strvec *list, const char *id)
{
    for (size_t i = 0; i < list->len; ++i)
        if (strcmp(list->items[i], id) == 0)
            return i + 1;
    return 0;
}

int action_create(const char *time_str, const char *after_id, const char *require_id)
{
    if (store_ensure_base() < 0)
    {
        fprintf(stderr, "Error: cannot create data dir at %s\n", store_base_dir());
        return 1;
    }

    trigger t;
    if (parse_trigger(time_str, after_id, require_id, &t) < 0)
        return 1;

    time_t now = time(NULL);

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
    {
        fprintf(stderr, "Error: getcwd: %s\n", strerror(errno));
        return 1;
    }

    print_task_header(&t, now, cwd);

    strvec *cmds = NULL;
    if (read_commands(&cmds) < 0)
    {
        fprintf(stderr, "Error: failed to read commands\n");
        strvec_free(&cmds);
        return 1;
    }
    if (cmds == NULL || cmds->len == 0)
    {
        fprintf(stderr, "Error: no commands provided\n");
        strvec_free(&cmds);
        return 1;
    }

    int rc = spawn_task(&t, now, cwd, cmds);
    strvec_free(&cmds);
    return rc;
}

int action_list(int verbose)
{
    enum
    {
        LIST_MAX = 15
    };

    if (store_ensure_base() < 0)
        return 1;

    strvec *list = NULL;
    if (store_list(&list) < 0)
    {
        fprintf(stderr, "Error: cannot list tasks\n");
        strvec_free(&list);
        return 1;
    }
    if (list->len == 0)
    {
        printf("No tasks found\n");
        strvec_free(&list);
        return 0;
    }

    if (verbose)
        printf("%-3s %-10s %-20s %-20s %-5s %-25s %s\n", "#", "Status", "Created at", "Execute at",
               "Cmds", "ID", "Preview");
    else
        printf("%-3s %-10s %-20s %-20s %s\n", "#", "Status", "Created at", "Execute at", "Cmds");

    size_t start = (!verbose && list->len > LIST_MAX) ? list->len - LIST_MAX : 0;
    for (size_t i = start; i < list->len; ++i)
    {
        const char *id = list->items[i];
        task_status st = store_resolve_status(id);

        task_meta meta;
        int have_meta = (store_read_meta(id, &meta) == 0);
        char created[64], scheduled[64];
        if (have_meta)
        {
            timefmt_format_time(meta.created_at, created, sizeof(created));
            if (meta.wait_for[0])
            {
                size_t dep = list_index_of(list, meta.wait_for);
                const char *kind = meta.wait_require ? "require" : "after";
                if (dep)
                    snprintf(scheduled, sizeof(scheduled), "%s #%zu", kind, dep);
                else
                    snprintf(scheduled, sizeof(scheduled), "%s #?", kind);
            }
            else
            {
                timefmt_format_time(meta.execute_at, scheduled, sizeof(scheduled));
            }
        }
        else
        {
            // orphan tasks have no meta; show them without meta info rather than hiding them
            snprintf(created, sizeof(created), "-");
            snprintf(scheduled, sizeof(scheduled), "-");
        }

        strvec *cmds = NULL;
        store_read_commands(id, &cmds);
        size_t ncmds = cmds ? cmds->len : 0;
        const char *first = (cmds && cmds->len > 0) ? cmds->items[0] : "";

        if (verbose)
        {
            char preview[24] = "";
            if (ncmds > 0)
            {
                snprintf(preview, sizeof(preview), "%.20s", first);
                if (strlen(first) > 20)
                {
                    preview[17] = '.';
                    preview[18] = '.';
                    preview[19] = '.';
                    preview[20] = '\0';
                }
            }
            printf("%-3zu %s%-10s%s %-20s %-20s %-5zu %-25s %s\n", i + 1,
                   store_status_color_prefix(st), store_status_name(st),
                   store_status_color_suffix(), created, scheduled, ncmds, id, preview);
        }
        else
        {
            printf("%-3zu %s%-10s%s %-20s %-20s %zu\n", i + 1, store_status_color_prefix(st),
                   store_status_name(st), store_status_color_suffix(), created, scheduled, ncmds);
        }
        strvec_free(&cmds);
    }
    strvec_free(&list);
    return 0;
}

int action_show(const char *id_input)
{
    char id[64];
    if (resolve_or_error(id_input, id, sizeof(id)) < 0)
        return 1;

    task_meta meta;
    if (store_read_meta(id, &meta) < 0)
    {
        fprintf(stderr, "Error: cannot read task %s\n", id);
        return 1;
    }
    task_status st = store_resolve_status(id);

    char created[64];
    timefmt_format_time(meta.created_at, created, sizeof(created));

    printf("Task: %s\n", meta.id);
    printf("Status:      %s%s%s\n", store_status_color_prefix(st), store_status_name(st),
           store_status_color_suffix());
    printf("Created at:  %s\n", created);
    if (meta.wait_for[0])
    {
        if (meta.wait_require)
            printf("Execute at:  requires task %s to complete\n", meta.wait_for);
        else
            printf("Execute at:  after task %s ends\n", meta.wait_for);
    }
    else
    {
        char scheduled[64], duration[64];
        timefmt_format_time(meta.execute_at, scheduled, sizeof(scheduled));
        timefmt_format_duration((long)(meta.execute_at - meta.created_at), duration,
                                sizeof(duration));
        printf("Execute at:  %s (%s)\n", scheduled, duration);
    }
    printf("Working dir: %s\n", meta.cwd);

    strvec *cmds = NULL;
    if (store_read_commands(id, &cmds) == 0)
    {
        printf("Commands:\n");
        for (size_t i = 0; i < cmds->len; ++i)
            printf("  %zu. %s\n", i + 1, cmds->items[i]);
    }
    strvec_free(&cmds);

    if (st == STATUS_FAILED)
    {
        char err[512];
        if (store_read_marker(id, "error", err, sizeof(err)) > 0)
            printf("Error: %s\n", err);
    }
    return 0;
}

int action_cancel(const char *id_input)
{
    char id[64];
    if (resolve_or_error(id_input, id, sizeof(id)) < 0)
        return 1;

    task_meta meta;
    if (store_read_meta(id, &meta) < 0)
    {
        fprintf(stderr, "Error: cannot read task %s\n", id);
        return 1;
    }

    task_status st = store_resolve_status(id);
    if (store_status_is_final(st))
    {
        printf("Task %s is already %s\n", id, store_status_name(st));
        return 0;
    }

    // record intent before freezing
    if (store_create_marker(id, "cancel") < 0)
    {
        fprintf(stderr, "Error: cannot record cancel intent for: %s: %s\n", id, strerror(errno));
        return 1;
    }

    if (meta.daemon_pid <= 0)
    {
        store_remove_marker(id, "cancel");
        fprintf(stderr, "Error: task %s has no daemon pid recorded\n", id);
        return 1;
    }

    // SIGCONT first in case the daemon is paused (SIGSTOP)
    kill(-meta.daemon_pid, SIGCONT);

    if (kill(-meta.daemon_pid, SIGTERM) < 0)
    {
        int saved = errno;
        store_remove_marker(id, "cancel");
        if (saved == ESRCH)
        {
            printf("Daemon %d already exited; nothing to cancel\n", meta.daemon_pid);
            return 0;
        }
        fprintf(stderr, "Error: cannot signal daemon %d: %s\n", meta.daemon_pid, strerror(errno));
        return 1;
    }

    int stuck = 0;
    strvec *one = NULL;
    if (strvec_init(&one) == 0 && strvec_push(one, id) == 0)
    {
        wait_then_kill(one);
        stuck = any_group_alive(one);
    }
    strvec_free(&one);

    char path[PATH_MAX];
    if (store_path_in_task(id, "log", path, sizeof(path)) == 0)
    {
        FILE *f = fopen(path, "a");
        if (f)
        {
            fprintf(f, "Task cancelled by user.\n");
            fclose(f);
        }
        else
        {
            fprintf(stderr, "Warning: cannot append cancel note to log: %s\n", strerror(errno));
        }
    }
    printf("Task %s cancelled\n", id);
    if (stuck)
        fprintf(stderr,
                "Warning: a process from %s could not be killed; it may be stuck (check with ps)\n",
                id);
    return 0;
}

int action_pause(const char *id_input)
{
    char id[64];
    if (resolve_or_error(id_input, id, sizeof(id)) < 0)
        return 1;

    task_meta meta;
    if (store_read_meta(id, &meta) < 0)
    {
        fprintf(stderr, "Error: cannot read task %s\n", id);
        return 1;
    }

    task_status st = store_resolve_status(id);
    if (st == STATUS_PAUSED)
    {
        printf("Task %s is already paused\n", id);
        return 0;
    }
    if (store_status_is_final(st))
    {
        fprintf(stderr, "Error: task %s is %s, cannot pause\n", id, store_status_name(st));
        return 1;
    }

    // record intent before freezing
    if (store_create_marker(id, "pause") < 0)
    {
        fprintf(stderr, "Error: cannot record pause intent for %s: %s\n", id, strerror(errno));
        return 1;
    }
    if (meta.daemon_pid <= 0)
    {
        store_remove_marker(id, "pause");
        fprintf(stderr, "Error: task %s has no daemon pid recorded\n", id);
        return 1;
    }

    if (kill(-meta.daemon_pid, SIGSTOP) < 0)
    {
        int saved = errno;
        store_remove_marker(id, "pause");
        if (saved == ESRCH)
        {
            printf("Daemon %d already exited; nothing to pause\n", meta.daemon_pid);
            return 0;
        }
        fprintf(stderr, "Error: cannot signal daemon %d: %s\n", meta.daemon_pid, strerror(errno));
        return 1;
    }
    printf("Task %s paused\n", id);
    return 0;
}

int action_resume(const char *id_input)
{
    char id[64];
    if (resolve_or_error(id_input, id, sizeof(id)) < 0)
        return 1;

    task_meta meta;
    if (store_read_meta(id, &meta) < 0)
    {
        fprintf(stderr, "Error: cannot read task %s\n", id);
        return 1;
    }

    if (store_resolve_status(id) != STATUS_PAUSED)
    {
        fprintf(stderr, "Error: task %s is not paused\n", id);
        return 1;
    }
    if (meta.daemon_pid <= 0)
    {
        fprintf(stderr, "Error: task %s has no daemon pid recorded\n", id);
        return 1;
    }

    if (kill(-meta.daemon_pid, SIGCONT) < 0)
    {
        if (errno == ESRCH)
        {
            printf("Daemon %d already exited; task is no longer running\n", meta.daemon_pid);
            return 0;
        }
        fprintf(stderr, "Error: cannot signal daemon %d: %s\n", meta.daemon_pid, strerror(errno));
        return 1;
    }
    store_remove_marker(id, "pause");
    printf("Task %s resumed\n", id);
    return 0;
}

int action_delete(const char *id_input)
{
    char id[64];
    if (resolve_or_error(id_input, id, sizeof(id)) < 0)
        return 1;

    task_status st = store_resolve_status(id);
    if (!store_status_is_final(st))
    {
        fprintf(stderr, "Error: task %s is still %s, cancel it first\n", id, store_status_name(st));
        return 1;
    }

    if (store_delete_task(id) < 0)
    {
        fprintf(stderr, "Error: failed to delete %s: %s\n", id, strerror(errno));
        return 1;
    }
    printf("Task %s deleted\n", id);
    return 0;
}

int action_log(const char *id_input, int verbose)
{
    char id[64];
    if (resolve_or_error(id_input, id, sizeof(id)) < 0)
        return 1;

    char path[PATH_MAX];
    if (store_path_in_task(id, "log", path, sizeof(path)) < 0)
        return 1;

    FILE *f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "Error: no log file for %s\n", id);
        return 1;
    }

    if (verbose)
    {
        char buf[4096];
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
            fwrite(buf, 1, r, stdout);
    }
    else
    {
        enum
        {
            TAIL_MAX = 100
        };
        char *ring[TAIL_MAX] = {0};
        size_t head = 0, count = 0;
        char *line = NULL;
        size_t cap = 0;
        ssize_t got;
        while ((got = getline(&line, &cap, f)) > 0)
        {
            free(ring[head]);
            ring[head] = strdup(line);
            head = (head + 1) % TAIL_MAX;
            if (count < TAIL_MAX)
                ++count;
        }
        free(line);
        size_t start = (count < TAIL_MAX) ? 0 : head;
        for (size_t i = 0; i < count; ++i)
        {
            size_t k = (start + i) % TAIL_MAX;
            if (ring[k])
                fputs(ring[k], stdout);
        }
        for (size_t i = 0; i < TAIL_MAX; ++i)
            free(ring[i]);
    }
    fclose(f);
    return 0;
}

int action_clean(void)
{
    if (store_ensure_base() < 0)
        return 1;

    strvec *list = NULL;
    if (store_list(&list) < 0)
    {
        fprintf(stderr, "Error: cannot list tasks\n");
        strvec_free(&list);
        return 1;
    }

    int n = 0;
    for (size_t i = 0; i < list->len; ++i)
    {
        if (store_status_is_final(store_resolve_status(list->items[i])))
        {
            if (store_delete_task(list->items[i]) == 0)
                ++n;
        }
    }
    strvec_free(&list);
    printf("Cleaned %d task(s)\n", n);
    return 0;
}

int action_retry(const char *id_input, const char *time_str, const char *after_id,
                 const char *require_id)
{
    if ((!time_str || !*time_str) && !after_id && !require_id)
    {
        fprintf(stderr, "Error: --retry requires a trigger (e.g., later --retry %s +0s)\n",
                id_input);
        return 1;
    }

    char id[64];
    if (resolve_or_error(id_input, id, sizeof(id)) < 0)
        return 1;

    strvec *cmds = NULL;
    if (store_read_commands(id, &cmds) < 0 || cmds->len == 0)
    {
        fprintf(stderr, "Error: task %s has no commands to retry\n", id);
        strvec_free(&cmds);
        return 1;
    }

    if (store_ensure_base() < 0)
    {
        fprintf(stderr, "Error: cannot create data dir at %s\n", store_base_dir());
        strvec_free(&cmds);
        return 1;
    }

    trigger t;
    if (parse_trigger(time_str, after_id, require_id, &t) < 0)
    {
        strvec_free(&cmds);
        return 1;
    }

    time_t now = time(NULL);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
    {
        fprintf(stderr, "Error: getcwd: %s\n", strerror(errno));
        strvec_free(&cmds);
        return 1;
    }

    // show the commands
    print_task_header(&t, now, cwd);
    printf("Commands:\n");
    for (size_t i = 0; i < cmds->len; ++i)
        printf("  %zu. %s\n", i + 1, cmds->items[i]);

    int rc = spawn_task(&t, now, cwd, cmds);
    strvec_free(&cmds);
    return rc;
}

int action_purge(void)
{
    strvec *list = NULL;
    if (store_list(&list) < 0)
    {
        fprintf(stderr, "Error: cannot list tasks\n");
        strvec_free(&list);
        return 1;
    }

    // stop every live task
    int stopped = 0;
    for (size_t i = 0; i < list->len; ++i)
    {
        if (!store_is_locked(list->items[i]))
            continue;
        task_meta meta;
        if (store_read_meta(list->items[i], &meta) == 0 && meta.daemon_pid > 0)
        {
            kill(-meta.daemon_pid, SIGCONT);
            kill(-meta.daemon_pid, SIGTERM);
            ++stopped;
        }
    }

    // wait up to 10s for the daemons to exit, then SIGKILL stragglers
    int forced = stopped > 0 ? wait_then_kill(list) : 0;
    // still alive means it survived even SIGKILL (e.g. stuck in D state)
    int stuck = stopped > 0 && any_group_alive(list);

    int removed = 0, failed = 0;
    for (size_t i = 0; i < list->len; ++i)
    {
        if (store_delete_task(list->items[i]) == 0)
            ++removed;
        else
            ++failed;
    }
    strvec_free(&list);

    strvec *foreign = NULL;
    store_list_foreign(&foreign);
    size_t nforeign = foreign ? foreign->len : 0;

    if (removed == 0 && stopped == 0 && nforeign == 0)
    {
        store_remove_base();
        printf("Nothing to purge.\n");
        strvec_free(&foreign);
        return 0;
    }

    printf("Purged %d task(s).\n", removed);
    if (forced > 0)
        printf("Force-killed %d unresponsive daemon(s).\n", forced);
    if (stuck)
        printf("Warning: some tasks could not be fully stopped; a process may be stuck (check with "
               "ps).\n");
    if (failed > 0)
        printf("Warning: %d task dir(s) could not be removed.\n", failed);

    if (nforeign == 0 && failed == 0)
    {
        if (store_remove_base() == 0)
            printf("Removed %s\n", store_base_dir());
    }
    else if (nforeign > 0)
    {
        printf("Left %zu item(s) not created by later in %s:\n", nforeign, store_base_dir());
        for (size_t i = 0; i < foreign->len; ++i)
            printf("  %s\n", foreign->items[i]);
        printf("Remove that directory manually to erase everything.\n");
    }
    strvec_free(&foreign);
    return failed > 0 ? 1 : 0;
}

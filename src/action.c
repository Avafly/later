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

static void print_task_header(time_t exec_at, time_t now, const char *cwd)
{
    char scheduled[64], duration[64];
    timefmt_format_time(exec_at, scheduled, sizeof(scheduled));
    timefmt_format_duration((long)(exec_at - now), duration, sizeof(duration));
    printf("Execute at:  %s (%s)\n", scheduled, duration);
    printf("Working dir: %s\n", cwd);
}

/* Fork the daemon and wait for its readiness signal.
 * Return 0 if the daemon reported success, or 1 on failure. */
static int spawn_task(time_t exec_at, time_t now, const char *cwd, const strvec *cmds)
{
    task_meta meta = {0};
    generate_id(meta.id, sizeof(meta.id));
    snprintf(meta.cwd, sizeof(meta.cwd), "%s", cwd);
    meta.created_at = now;
    meta.execute_at = exec_at;
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

/* Return 1 if any task's process group still has a live member. */
static int any_group_alive(const strvec *tasks)
{
    for (size_t i = 0; i < tasks->len; ++i)
    {
        task_meta meta;
        if (store_read_meta(tasks->items[i], &meta) == 0 && meta.daemon_pid > 0 &&
            kill(-meta.daemon_pid, 0) == 0)
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
        if (store_read_meta(tasks->items[i], &meta) == 0 && meta.daemon_pid > 0 &&
            kill(-meta.daemon_pid, 0) == 0)
        {
            kill(-meta.daemon_pid, SIGKILL);
            ++n;
        }
    }
    return n;
}

int action_create(const char *time_str)
{
    if (store_ensure_base() < 0)
    {
        fprintf(stderr, "Error: cannot create data dir at %s\n", store_base_dir());
        return 1;
    }

    char errbuf[256];
    time_t exec_at;
    if (timefmt_parse_time(time_str, &exec_at, errbuf, sizeof(errbuf)) < 0)
    {
        fprintf(stderr, "Error: %s\n", errbuf);
        return 1;
    }

    time_t now = time(NULL);

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
    {
        fprintf(stderr, "Error: getcwd: %s\n", strerror(errno));
        return 1;
    }

    print_task_header(exec_at, now, cwd);

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

    int rc = spawn_task(exec_at, now, cwd, cmds);
    strvec_free(&cmds);
    return rc;
}

int action_list(int verbose)
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

    for (size_t i = 0; i < list->len; ++i)
    {
        const char *id = list->items[i];
        task_status st = store_resolve_status(id);

        task_meta meta;
        int have_meta = (store_read_meta(id, &meta) == 0);
        char created[64], scheduled[64];
        if (have_meta)
        {
            timefmt_format_time(meta.created_at, created, sizeof(created));
            timefmt_format_time(meta.execute_at, scheduled, sizeof(scheduled));
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

    char scheduled[64], duration[64], created[64];
    timefmt_format_time(meta.execute_at, scheduled, sizeof(scheduled));
    timefmt_format_duration((long)(meta.execute_at - meta.created_at), duration, sizeof(duration));
    timefmt_format_time(meta.created_at, created, sizeof(created));

    printf("Task: %s\n", meta.id);
    printf("Status:      %s%s%s\n", store_status_color_prefix(st), store_status_name(st),
           store_status_color_suffix());
    printf("Created at:  %s\n", created);
    printf("Execute at:  %s (%s)\n", scheduled, duration);
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
            fprintf(stderr, "Warning: cannot append cancel note to log: %s", strerror(errno));
        }
    }
    printf("Task %s cancelled\n", id);
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

int action_retry(const char *id_input, const char *time_str)
{
    if (!time_str || !*time_str)
    {
        fprintf(stderr, "Error: --retry requires a time argument (e.g., later --retry %s +0s)\n",
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

    char errbuf[256];
    time_t exec_at;
    if (timefmt_parse_time(time_str, &exec_at, errbuf, sizeof(errbuf)) < 0)
    {
        fprintf(stderr, "Error: %s\n", errbuf);
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
    print_task_header(exec_at, now, cwd);
    printf("Commands:\n");
    for (size_t i = 0; i < cmds->len; ++i)
        printf("  %zu. %s\n", i + 1, cmds->items[i]);

    int rc = spawn_task(exec_at, now, cwd, cmds);
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

    // wait up to 10s for the daemons to exit
    int forced = 0;
    if (stopped > 0)
    {
        int waited = 0;
        while (waited < 10000 && any_group_alive(list))
        {
            sleep_ms(100);
            waited += 100;
        }
        if (any_group_alive(list))
        {
            forced = kill_alive_groups(list);
            for (int k = 0; k < 10 && any_group_alive(list); ++k)
                sleep_ms(100);
        }
    }

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

#include "action.h"

#include "daemon.h"
#include "exec.h"
#include "store.h"
#include "timefmt.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int resolve_or_error(const char *input, char *out, size_t n)
{
    int rc = id_resolve(input, out, n);
    if (rc == -1)
        fprintf(stderr, "Error: task '%s' not found\n", input);
    else if (rc == -2)
        fprintf(stderr, "Error: task '%s' is ambiguous\n", input);
    return rc;
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
    if (parse_time(time_str, &exec_at, errbuf, sizeof(errbuf)) < 0)
    {
        fprintf(stderr, "Error: %s\n", errbuf);
        return 1;
    }

    time_t now = time(NULL);
    char tbuf[64], dbuf[64];
    format_time(exec_at, tbuf, sizeof(tbuf));
    format_duration((long)(exec_at - now), dbuf, sizeof(dbuf));

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
    {
        fprintf(stderr, "Error: getcwd: %s\n", strerror(errno));
        return 1;
    }

    printf("Execute at:  %s (%s)\n", tbuf, dbuf);
    printf("Working dir: %s\n", cwd);

    strvec cmds;
    strvec_init(&cmds);
    if (read_commands(&cmds) < 0 || cmds.len == 0)
    {
        fprintf(stderr, "Error: no commands provided\n");
        strvec_free(&cmds);
        return 1;
    }

    task_meta meta = {0};
    id_generate(meta.id, sizeof(meta.id));
    snprintf(meta.cwd, sizeof(meta.cwd), "%s", cwd);
    meta.created_at = now;
    meta.execute_at = exec_at;
    meta.daemon_pid = -1; /* filled in by the daemon */

    int pipefd[2];
    if (pipe(pipefd) < 0)
    {
        fprintf(stderr, "Error: pipe: %s\n", strerror(errno));
        strvec_free(&cmds);
        return 1;
    }

    /* drain stdio buffers before fork: if stdout is block-buffered (piped),
     * the parent's pre-fork prints sit in the FILE buffer and the daemon
     * inherits them — they would then leak into the log file when the
     * daemon flushes during execution. */
    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "Error: fork: %s\n", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        strvec_free(&cmds);
        return 1;
    }

    if (pid == 0)
    {
        /* child: become the daemon (never returns) */
        close(pipefd[0]);
        daemon_run(&meta, cmds.items, cmds.len, pipefd[1]);
    }

    /* parent: wait for the daemon to report readiness */
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

    /* reap the first-fork child so it doesn't linger as a zombie. */
    int wstatus;
    waitpid(pid, &wstatus, 0);

    int ret;
    if (off > 0 && report[0] == 'k')
    {
        printf("Task %s created\n", meta.id);
        ret = 0;
    }
    else if (off > 0 && report[0] == 'e')
    {
        fprintf(stderr, "Error: %s\n", report + 1);
        ret = 1;
    }
    else
    {
        fprintf(stderr, "Error: daemon failed to start\n");
        ret = 1;
    }

    strvec_free(&cmds);
    return ret;
}

int action_list(int verbose)
{
    if (store_ensure_base() < 0)
        return 1;

    strvec list;
    if (store_list(&list) < 0)
    {
        fprintf(stderr, "Error: cannot list tasks\n");
        return 1;
    }
    if (list.len == 0)
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

    for (size_t i = 0; i < list.len; ++i)
    {
        const char *id = list.items[i];
        task_meta meta;
        if (store_read_meta(id, &meta) < 0)
            continue;

        task_status st = store_resolve_status(id);
        char created[64], scheduled[64];
        format_time(meta.created_at, created, sizeof(created));
        format_time(meta.execute_at, scheduled, sizeof(scheduled));

        strvec cmds;
        store_read_commands(id, &cmds);

        if (verbose)
        {
            char preview[24] = "";
            if (cmds.len > 0)
            {
                snprintf(preview, sizeof(preview), "%.20s", cmds.items[0]);
                if (strlen(cmds.items[0]) > 20)
                {
                    preview[17] = '.';
                    preview[18] = '.';
                    preview[19] = '.';
                    preview[20] = '\0';
                }
            }
            /* %-19s on the colored status keeps column alignment by counting
             * the ANSI escapes; status_name_color outputs 9 extra bytes. */
            printf("%-3zu %-19s %-20s %-20s %-5zu %-25s %s\n", i + 1, status_name_color(st),
                   created, scheduled, cmds.len, id, preview);
        }
        else
        {
            printf("%-3zu %-19s %-20s %-20s %zu\n", i + 1, status_name_color(st), created,
                   scheduled, cmds.len);
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

    char tbuf[64], dbuf[64];
    format_time(meta.execute_at, tbuf, sizeof(tbuf));
    format_duration((long)(meta.execute_at - meta.created_at), dbuf, sizeof(dbuf));

    char created[64];
    format_time(meta.created_at, created, sizeof(created));

    printf("Task: %s\n", meta.id);
    printf("Status:      %s\n", status_name_color(st));
    printf("Created at:  %s\n", created);
    printf("Execute at:  %s (%s)\n", tbuf, dbuf);
    printf("Working dir: %s\n", meta.cwd);

    strvec cmds;
    if (store_read_commands(id, &cmds) == 0)
    {
        printf("Commands:\n");
        for (size_t i = 0; i < cmds.len; ++i)
            printf("  %zu. %s\n", i + 1, cmds.items[i]);
        strvec_free(&cmds);
    }

    if (st == STATUS_FAILED)
    {
        char err[512];
        if (store_marker_read(id, "error", err, sizeof(err)) > 0)
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
    if (st != STATUS_PENDING && st != STATUS_RUNNING && st != STATUS_PAUSED)
    {
        printf("Task %s is already %s\n", id, status_name(st));
        return 0;
    }

    /* Record intent BEFORE signalling: if the daemon dies between SIGTERM
     * and the next observer, the marker still tells us "this was cancelled,
     * not crashed". The marker is rolled back if signalling fails so a
     * future status query doesn't misreport a still-running task as
     * Cancelled. */
    if (store_create_marker(id, "cancel") < 0)
    {
        fprintf(stderr, "Error: cannot record cancel intent for %s: %s\n", id, strerror(errno));
        return 1;
    }

    if (meta.daemon_pid <= 0)
    {
        store_marker_remove(id, "cancel");
        fprintf(stderr, "Error: task %s has no daemon pid recorded\n", id);
        return 1;
    }

    /* SIGCONT first in case the daemon is paused (SIGSTOP'd); a stopped
     * process can't deliver SIGTERM until it's running again. Failure
     * here is harmless — the daemon may simply not be stopped. */
    kill(-meta.daemon_pid, SIGCONT);

    if (kill(-meta.daemon_pid, SIGTERM) < 0)
    {
        int saved = errno;
        store_marker_remove(id, "cancel");
        if (saved == ESRCH)
        {
            /* Daemon vanished between our resolve_status and the kill.
             * Don't leave a misleading cancel marker; let resolve_status
             * report whatever the daemon actually managed to write. */
            printf("Daemon %d already exited; nothing to cancel\n", meta.daemon_pid);
            return 0;
        }
        fprintf(stderr, "Error: cannot signal daemon %d: %s\n", meta.daemon_pid, strerror(saved));
        return 1;
    }

    /* Cancellation has effectively taken hold — the log note is a
     * courtesy. Both writers open with O_APPEND so any racing daemon
     * write stays atomic. */
    char log_path[PATH_MAX];
    if (store_path_in_task(id, "log", log_path, sizeof(log_path)) == 0)
    {
        FILE *f = fopen(log_path, "a");
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
    return 0;
}

int action_delete(const char *id_input)
{
    char id[64];
    if (resolve_or_error(id_input, id, sizeof(id)) < 0)
        return 1;

    task_status st = store_resolve_status(id);
    if (st == STATUS_PENDING || st == STATUS_RUNNING || st == STATUS_PAUSED)
    {
        fprintf(stderr, "Error: task %s is still %s, cancel it first\n", id, status_name(st));
        return 1;
    }

    if (store_delete(id) < 0)
    {
        fprintf(stderr, "Error: failed to delete %s: %s\n", id, strerror(errno));
        return 1;
    }
    printf("Task %s deleted\n", id);
    return 0;
}

int action_logs(const char *id_input, int verbose)
{
    char id[64];
    if (resolve_or_error(id_input, id, sizeof(id)) < 0)
        return 1;

    char log_path[PATH_MAX];
    if (store_path_in_task(id, "log", log_path, sizeof(log_path)) < 0)
        return 1;

    FILE *f = fopen(log_path, "r");
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
        /* tail -100: simple ring buffer of recent lines */
        enum
        {
            MAX = 100
        };
        char *ring[MAX] = {0};
        size_t head = 0, count = 0;
        char *line = NULL;
        size_t cap = 0;
        ssize_t got;
        while ((got = getline(&line, &cap, f)) > 0)
        {
            free(ring[head]);
            ring[head] = strdup(line);
            head = (head + 1) % MAX;
            if (count < MAX)
                count++;
        }
        free(line);
        size_t start = (count < MAX) ? 0 : head;
        for (size_t i = 0; i < count; ++i)
        {
            size_t k = (start + i) % MAX;
            if (ring[k])
                fputs(ring[k], stdout);
        }
        for (size_t i = 0; i < MAX; ++i)
            free(ring[i]);
    }
    fclose(f);
    return 0;
}

int action_clean(void)
{
    if (store_ensure_base() < 0)
        return 1;

    strvec list;
    if (store_list(&list) < 0)
    {
        fprintf(stderr, "Error: cannot list tasks\n");
        return 1;
    }

    int n = 0;
    for (size_t i = 0; i < list.len; ++i)
    {
        if (status_is_final(store_resolve_status(list.items[i])))
        {
            if (store_delete(list.items[i]) == 0)
                n++;
        }
    }
    strvec_free(&list);
    printf("Cleaned %d task(s)\n", n);
    return 0;
}

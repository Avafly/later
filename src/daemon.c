#include "daemon.h"

#include "exec.h"
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Write data fully, retrying on partial writes and EINTR. Best-effort: a
 * lost readiness signal turns into a "Daemon failed to start" on the parent. */
static void write_all(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = write(fd, data + off, len - off);
        if (n > 0)
        {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return;
    }
}

static void report_and_exit(int ready_fd, const char *msg)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "e%s", msg);
    if (n > 0)
        write_all(ready_fd, buf, (size_t)n);
    close(ready_fd);
    _exit(1);
}

/* Sleep until the wall-clock deadline. Re-checks on every iteration so a
 * clock jump or EINTR can't cause an early run; the small extra cycles cost
 * nothing because we only loop on early wake-ups. */
static void sleep_until_wall(time_t deadline)
{
    while (1)
    {
        time_t now = time(NULL);
        if (now >= deadline)
            return;
        struct timespec ts;
        ts.tv_sec = deadline - now;
        ts.tv_nsec = 0;
        nanosleep(&ts, NULL);
    }
}

_Noreturn void daemon_run(const task_meta_t *meta_in, char *const cmds[], size_t ncmds,
                          int ready_fd)
{
    task_meta_t meta = *meta_in;

    /* first fork already happened in the parent: we are the child. */

    if (setsid() < 0)
        report_and_exit(ready_fd, strerror(errno));

    pid_t pid = fork();
    if (pid < 0)
        report_and_exit(ready_fd, strerror(errno));
    if (pid > 0)
        _exit(0); /* intermediate exits; grandchild continues */

    /* now the real daemon. */
    meta.daemon_pid = getpid();

    /* own a fresh process group so cancel can signal the whole task tree. */
    setpgid(0, 0);

    /* claim the task directory; daemon is the only creator. */
    char dir[PATH_MAX];
    if (store_task_dir(meta.id, dir, sizeof(dir)) < 0)
        report_and_exit(ready_fd, "task path too long");
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
    {
        char buf[PATH_MAX + 64];
        snprintf(buf, sizeof(buf), "mkdir %s: %s", dir, strerror(errno));
        report_and_exit(ready_fd, buf);
    }

    int lock_fd = store_acquire_lock(meta.id);
    if (lock_fd < 0)
        report_and_exit(ready_fd, "failed to acquire lock");

    /* chdir to / so we don't pin a mount point. */
    if (chdir("/") < 0)
        report_and_exit(ready_fd, strerror(errno));
    umask(0022);

    /* redirect stdio: stdin → /dev/null, stdout/stderr → log file. */
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull < 0)
        report_and_exit(ready_fd, "open /dev/null");
    if (dup2(devnull, STDIN_FILENO) < 0)
        report_and_exit(ready_fd, "dup2 stdin");
    close(devnull);

    char log_path[PATH_MAX];
    if (store_path_in_task(meta.id, "log", log_path, sizeof(log_path)) < 0)
        report_and_exit(ready_fd, "log path too long");
    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0)
        report_and_exit(ready_fd, strerror(errno));
    if (dup2(log_fd, STDOUT_FILENO) < 0 || dup2(log_fd, STDERR_FILENO) < 0)
        report_and_exit(ready_fd, "dup2 log");
    close(log_fd);

    /* persist meta and commands; both are atomic via tmp+rename. */
    if (store_write_meta(&meta) < 0)
        report_and_exit(ready_fd, "write meta");
    if (store_write_commands(meta.id, cmds, ncmds) < 0)
        report_and_exit(ready_fd, "write commands");

    /* readiness: from this point the task is fully observable. */
    write_all(ready_fd, "k", 1);
    close(ready_fd);

    sleep_until_wall(meta.execute_at);

    /* mark running BEFORE the first command starts. */
    if (store_create_marker(meta.id, "running") < 0)
    {
        store_create_marker_with_content(meta.id, "error", "failed to create running marker");
        store_fsync_marker(meta.id, "error");
        close(lock_fd);
        _exit(1);
    }

    int rc = exec_run_commands(cmds, ncmds, meta.cwd);

    /* Order: create terminal marker → fsync → close lock_fd.
     * The fsync guarantees an observer that sees the lock released will also
     * see the done/error marker on disk, so resolve_status never misreports
     * Completed/Failed as Failed-by-crash. */
    if (rc == 0)
    {
        store_create_marker(meta.id, "done");
        store_fsync_marker(meta.id, "done");
        close(lock_fd);
        _exit(0);
    }
    else
    {
        char msg[256];
        if (rc < 0)
            snprintf(msg, sizeof(msg), "execution error: %s", strerror(errno));
        else
            snprintf(msg, sizeof(msg), "Exit code: %d", rc);
        store_create_marker_with_content(meta.id, "error", msg);
        store_fsync_marker(meta.id, "error");
        close(lock_fd);
        _exit(1);
    }
}

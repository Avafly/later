#include "exec.h"

#include "timefmt.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int run_one(const char *cmd, const char *cwd)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (cwd && cwd[0] && chdir(cwd) < 0) {
            fprintf(stderr, "Failed to chdir to %s: %s\n", cwd, strerror(errno));
            _exit(126);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        fprintf(stderr, "Failed to exec /bin/sh: %s\n", strerror(errno));
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        fprintf(stderr, "Command killed by signal %d\n", sig);
        return 128 + sig;
    }
    return -1;
}

int exec_run_commands(char *const cmds[], size_t n, const char *cwd)
{
    for (size_t i = 0; i < n; ++i) {
        char ts[64];
        format_time(time(NULL), ts, sizeof(ts));
        printf("[%s] [%zu/%zu] %s\n", ts, i + 1, n, cmds[i]);
        fflush(stdout);

        int rc = run_one(cmds[i], cwd);
        if (rc < 0)
            return -1;
        if (rc != 0) {
            fprintf(stderr, "Command failed with exit code: %d\n", rc);
            fflush(stderr);
            return rc;
        }
    }

    if (n > 0) {
        char ts[64];
        format_time(time(NULL), ts, sizeof(ts));
        printf("[%s] All commands completed successfully\n", ts);
        fflush(stdout);
    }
    return 0;
}

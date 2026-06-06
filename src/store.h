#ifndef LATER_STORE_H_
#define LATER_STORE_H_

#include "strvec.h"

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

/*
 * Layout: $XDG_DATA_HOME/later/<id>/ (one directory per task)
 *   meta        immutable, key=value: cwd, created_at, execute_at, daemon_pid
 *   commands    immutable, one shell command per line (no '\n' allowed)
 *   log         stdout + stderr of the task
 *   lock        held by the daemon via flock; release on exit = "daemon gone"
 *   running     marker: created when the daemon starts the first command
 *   done        marker: created after all commands exit 0 (terminal: Completed)
 *   error       marker with content: failure reason (terminal: Failed)
 *   cancel      marker: created by `later --cancel` before signalling the daemon
 *   pause       marker: created by `later --pause` before SIGSTOP
 *
 * State invariant: every marker file is created at most once, never modified.
 * Each transition is a single atomic syscall (open O_EXCL or rename). Order
 * matters and is the daemon's responsibility (see daemon.c).
 */

typedef enum
{
    STATUS_PENDING,
    STATUS_RUNNING,
    STATUS_COMPLETED,
    STATUS_FAILED,
    STATUS_CANCELLED,
    STATUS_PAUSED
} task_status;

typedef struct
{
    char id[64];
    char cwd[PATH_MAX];
    time_t created_at;
    time_t execute_at;
    pid_t daemon_pid;
} task_meta;

/* Base dir: $XDG_DATA_HOME/later or $HOME/.local/share/later */
int store_ensure_base(void);
const char *store_base_dir(void);
int store_task_dir(const char *id, char *buf, size_t n);
int store_path_in_task(const char *id, const char *name, char *buf, size_t n);

/* Daemon liveness via advisory file lock. */
int store_acquire_lock(const char *id);
int store_is_locked(const char *id);

int store_write_meta(const task_meta *meta);
int store_read_meta(const char *id, task_meta *meta);

int store_write_commands(const char *id, char *const *cmds, size_t n);
int store_read_commands(const char *id, strvec **cmds);

/* Marker functions */
int store_create_marker(const char *id, const char *name);
int store_create_marker_with_content(const char *id, const char *name, const char *content);
int store_has_marker(const char *id, const char *name);
ssize_t store_read_marker(const char *id, const char *name, char *buf, size_t n);
int store_remove_marker(const char *id, const char *name);

/* Allocate *list and fill it with task ids sorted by created_at. */
int store_list(strvec **list);

task_status store_resolve_status(const char *id);

const char *store_status_name(task_status st);
const char *store_status_color_prefix(task_status st);
const char *store_status_color_suffix(void);
int store_status_is_final(task_status st);

/* Recursive rm of the task directory. */
int store_delete_task(const char *id);

int store_list_foreign(strvec **foreign);

int store_remove_base(void);

#endif // LATER_STORE_H_

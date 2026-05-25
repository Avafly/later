#ifndef LATER_STORE_H
#define LATER_STORE_H

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
    STATUS_PAUSED,
} task_status;

typedef struct
{
    char id[64];
    char cwd[PATH_MAX];
    time_t created_at;
    time_t execute_at;
    pid_t daemon_pid;
} task_meta;

/* base dir is $XDG_DATA_HOME/later or $HOME/.local/share/later; created lazily */
int store_ensure_base(void);
const char *store_base_dir(void);
int store_task_dir(const char *id, char *buf, size_t n);
int store_path_in_task(const char *id, const char *name, char *buf, size_t n);

/* return list of task ids sorted by created_at (caller frees via store_list_free) */
typedef struct
{
    char **ids;
    size_t len;
} task_id_list;
int store_list(task_id_list *out);
void store_list_free(task_id_list *list);

/* meta is written exactly once, atomically (tmp + rename). */
int store_write_meta(const task_meta *m);
int store_read_meta(const char *id, task_meta *out);

/* commands written once; cmd lines must not contain '\n'. */
int store_write_commands(const char *id, char *const *cmds, size_t n);
int store_read_commands(const char *id, char ***out_cmds, size_t *out_n);
void store_free_commands(char **cmds, size_t n);

/* marker primitives — all O_EXCL or unlink, no in-place modification. */
int store_create_marker(const char *id, const char *name);
int store_create_marker_with_content(const char *id, const char *name, const char *content);
int store_marker_exists(const char *id, const char *name);
/* returns bytes read (0 if missing), -1 on error; buf is NUL-terminated. */
ssize_t store_marker_read(const char *id, const char *name, char *buf, size_t n);
int store_marker_remove(const char *id, const char *name);

/* daemon liveness via advisory file lock. */
int store_acquire_lock(const char *id); /* keeps fd open; caller closes at exit */
int store_is_locked(const char *id);    /* 1 if some process holds it */

/* fsync a marker file by name (used by daemon before releasing the lock). */
int store_fsync_marker(const char *id, const char *name);

/* the authoritative status derivation. Reads markers + lock; no caching. */
task_status store_resolve_status(const char *id);

/* recursive rm of the task directory. */
int store_delete(const char *id);

const char *status_name(task_status s);
const char *status_name_color(task_status s);
int status_is_final(task_status s);

#endif

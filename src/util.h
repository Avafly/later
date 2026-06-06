#ifndef LATER_UTIL_H_
#define LATER_UTIL_H_

#include "strvec.h"

#include <stddef.h>
#include <sys/types.h>

/* mkdir(path, mode), tolerating EEXIST if the existing path is a directory.
 * Return 0 on success or pre-existing directory; -1 with errno set otherwise
 * (errno = ENOTDIR if the path exists but isn't a directory). */
int ensure_dir(const char *path, mode_t mode);

/* Read commands from stdin: tty -> linenoise prompt, pipe -> one line each.
 * Return 0 on success (may be empty), -1 on OOM. */
int read_commands(strvec **cmds);

/* "<sec>_<pid>_<rand4hex>"; buf must be >= 64 bytes. */
void generate_id(char *buf, size_t n);

/* Resolve user input to a task id.
 * Return 0 on success, -1 if not found, or -2 if ambiguous. */
int resolve_id(const char *input, char *out, size_t n);

/* Return 1 if name has the shape generate_id produces <digits>_<digits>_<hex>. */
int is_task_id(const char *name);

/* Recursive directory removal. */
int rm_rf(const char *path);

#endif // LATER_UTIL_H_

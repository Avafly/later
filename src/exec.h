#ifndef LATER_EXEC_H_
#define LATER_EXEC_H_

#include <stddef.h>

/*
 * Run each command via /bin/sh -c, with cwd as the working directory.
 * Stops at the first non-zero exit; the daemon decides what to do next.
 *
 * Returns:
 *   0           every command succeeded
 *   >0          exit code of the failed command
 *   -1          fork/wait failure (errno set); caller should treat as failure
 */
int exec_run_commands(char *const cmds[], size_t n, const char *cwd);

#endif // LATER_EXEC_H_

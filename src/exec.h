#ifndef LATER_EXEC_H_
#define LATER_EXEC_H_

#include <stddef.h>

/* Run each command via /bin/sh -c with cwd as the working directory.
 * Return 0 if all commands succeed, -1 on fork/wait failure, or the exit code of the failed
 * command. */
int exec_run_commands(char *const *cmds, size_t n, const char *cwd);

#endif // LATER_EXEC_H_

#ifndef LATER_DAEMON_H
#define LATER_DAEMON_H

#include "store.h"

/*
 * Take over the child after fork(): daemonize (double-fork + setsid), claim
 * the task directory and lock, write meta/commands, signal readiness on
 * ready_fd, then run the lifecycle until done/error and exit.
 *
 * cmds is owned by the caller's heap and survives until _exit; the daemon
 * does not free it.
 */
_Noreturn void daemon_run(const task_meta_t *meta, char *const cmds[],
                          size_t ncmds, int ready_fd);

#endif

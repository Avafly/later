#ifndef LATER_DAEMON_H_
#define LATER_DAEMON_H_

#include "store.h"

void daemon_run(task_meta meta, char *const *cmds, size_t ncmds, int ready_fd);

#endif // LATER_DAEMON_H_

#ifndef UTILS_H_
#define UTILS_H_

#include "task.h"

#include <optional>
#include <string>

namespace later
{

// sort tasks by execution time (ascending)
bool TaskSorter(const Task &a, const Task &b);

// print formatted task information
void PrintTaskInfo(const Task &task);

// resolve task ID from index (1-based) or prefix match
std::optional<std::string> ResolveTaskId(const std::string &id);

// read commands interactively using linenoise
std::vector<std::string> ReadCommands();

// generate a unique task ID (timestamp_pid)
std::string GenerateTaskId();

} // namespace later

#endif // UTILS_H_
#ifndef EXECUTOR_H_
#define EXECUTOR_H_

#include "task.h"

#include "3rdparty/expected.hpp"

#include <string>

namespace later
{

// execute all commands in task sequentially
tl::expected<int, std::string> ExecuteCommands(const Task &task);

} // namespace later

#endif // EXECUTOR_H_
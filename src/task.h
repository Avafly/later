#ifndef TASK_H_
#define TASK_H_

#include "3rdparty/json.hpp"

#include <sys/types.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace later
{

enum class TaskStatus
{
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled
};

struct Task
{
    std::string id;
    std::string cwd;
    std::vector<std::string> commands;

    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point execute_at;

    pid_t daemon_pid{-1};
    TaskStatus status{TaskStatus::Pending};
    std::string error_message;

    nlohmann::json ToJson() const;
    static Task FromJson(const nlohmann::json &j);
};

std::string TaskStatusToString(TaskStatus status, bool is_color = false);
TaskStatus TaskStatusFromString(std::string_view str);
bool IsFinalStatus(TaskStatus status);

} // namespace later

#endif // TASK_H_
#include "task.h"

#include "3rdparty/json.hpp"

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace later
{

nlohmann::json Task::ToJson() const
{
    return nlohmann::json{
        {"id", id},
        {"cwd", cwd},
        {"commands", commands},
        {"created_at",
         std::chrono::duration_cast<std::chrono::seconds>(created_at.time_since_epoch()).count()},
        {"execute_at",
         std::chrono::duration_cast<std::chrono::seconds>(execute_at.time_since_epoch()).count()},
        {"daemon_pid", daemon_pid},
        {"status", TaskStatusToString(status)},
        {"error_message", error_message}};
}

Task Task::FromJson(const nlohmann::json &j)
{
    Task task;
    task.id = j.at("id").get<std::string>();
    task.cwd = j.at("cwd").get<std::string>();
    task.commands = j.at("commands").get<std::vector<std::string>>();
    task.created_at = std::chrono::system_clock::time_point{
        std::chrono::seconds{j.at("created_at").get<int64_t>()}};
    task.execute_at = std::chrono::system_clock::time_point{
        std::chrono::seconds{j.at("execute_at").get<int64_t>()}};
    task.daemon_pid = j.at("daemon_pid").get<pid_t>();
    task.status = TaskStatusFromString(j.at("status").get<std::string>());
    task.error_message = j.value("error_message", "");
    return task;
}

std::string TaskStatusToString(TaskStatus status, bool is_color)
{
    if (is_color)
    {
        // format task status with ANSI colors
        switch (status)
        {
            case TaskStatus::Pending:
                return "\033[33mpending\033[0m";
            case TaskStatus::Running:
                return "\033[34mrunning\033[0m";
            case TaskStatus::Completed:
                return "\033[32mcompleted\033[0m";
            case TaskStatus::Failed:
                return "\033[31mfailed\033[0m";
            case TaskStatus::Cancelled:
                return "\033[90mcancelled\033[0m";
            default:
                return "unknown";
        }
    }
    else
    {
        switch (status)
        {
            case TaskStatus::Pending:
                return "pending";
            case TaskStatus::Running:
                return "running";
            case TaskStatus::Completed:
                return "completed";
            case TaskStatus::Failed:
                return "failed";
            case TaskStatus::Cancelled:
                return "cancelled";
            default:
                return "unknown";
        }
    }
}

TaskStatus TaskStatusFromString(std::string_view str)
{
    if (str == "pending")
        return TaskStatus::Pending;
    if (str == "running")
        return TaskStatus::Running;
    if (str == "completed")
        return TaskStatus::Completed;
    if (str == "failed")
        return TaskStatus::Failed;
    if (str == "cancelled")
        return TaskStatus::Cancelled;
    return TaskStatus::Pending;
}

bool IsFinalStatus(TaskStatus status)
{
    return status == TaskStatus::Completed || status == TaskStatus::Failed ||
           status == TaskStatus::Cancelled;
}

} // namespace later
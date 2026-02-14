#ifndef STORAGE_H_
#define STORAGE_H_

#include "task.h"

#include "3rdparty/expected.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace later
{

class Storage
{
public:
    Storage();

    void SaveTask(const Task &task);
    tl::expected<Task, std::string> LoadTask(std::string_view id);
    std::vector<Task> ListTasks();
    void DeleteTask(std::string_view id);
    void UpdateTaskStatus(std::string_view id, TaskStatus status, std::string_view error_msg = "");
    TaskStatus ResolveTaskStatus(const Task &task);
    int CleanFinishedTasks();

    std::filesystem::path GetLogPath(std::string_view id);
    std::filesystem::path GetTaskPath(std::string_view id);
    std::filesystem::path GetLockPath(std::string_view id);

    // lock file management for daemon liveness check
    int AcquireDaemonLock(std::string_view id);
    bool IsDaemonLocked(std::string_view id);

private:
    std::filesystem::path base_dir_;
    std::filesystem::path tasks_dir_;
    std::filesystem::path logs_dir_;
    std::filesystem::path locks_dir_;
};

} // namespace later

#endif // STORAGE_H_
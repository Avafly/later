#include "storage.h"

#include "task.h"

#include "3rdparty/expected.hpp"
#include "3rdparty/fmt/base.h"
#include "3rdparty/fmt/format.h"
#include "3rdparty/json.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace later
{

Storage::Storage()
{
    const char *xdg_data = std::getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0] != '\0')
    {
        base_dir_ = std::filesystem::path(xdg_data) / "later";
    }
    else
    {
        const char *home = std::getenv("HOME");
        if (!home)
            throw std::runtime_error("HOME environment variable not set");
        base_dir_ = std::filesystem::path(home) / ".local" / "share" / "later";
    }
    tasks_dir_ = base_dir_ / "tasks";
    logs_dir_ = base_dir_ / "logs";
    locks_dir_ = base_dir_ / "locks";

    std::filesystem::create_directories(tasks_dir_);
    std::filesystem::create_directories(logs_dir_);
    std::filesystem::create_directories(locks_dir_);
}

void Storage::SaveTask(const Task &task)
{
    auto path = GetTaskPath(task.id);
    std::filesystem::create_directories(path.parent_path());

    std::ofstream file(path);
    if (!file)
        throw std::runtime_error(fmt::format("Failed to open task file: {}", path.string()));

    file << task.ToJson().dump(2);
    if (!file)
        throw std::runtime_error(fmt::format("Failed to write task file: {}", path.string()));
}

tl::expected<Task, std::string> Storage::LoadTask(std::string_view id)
{
    auto path = GetTaskPath(id);
    std::ifstream file(path);
    if (!file)
        return tl::unexpected(fmt::format("Failed to open task file {}", path.string()));

    try
    {
        nlohmann::json j;
        file >> j;
        return Task::FromJson(j);
    }
    catch (const nlohmann::json::exception &e)
    {
        return tl::unexpected(fmt::format("Corrupt task file in {}: {}", path.string(), e.what()));
    }
}

std::vector<Task> Storage::ListTasks()
{
    std::vector<Task> tasks;

    if (!std::filesystem::exists(tasks_dir_))
        return tasks;

    for (const auto &entry : std::filesystem::directory_iterator(tasks_dir_))
    {
        if (entry.path().extension() != ".json")
            continue;

        std::ifstream file(entry.path());
        if (!file)
            continue;

        try
        {
            nlohmann::json j;
            file >> j;
            tasks.emplace_back(Task::FromJson(j));
        }
        catch (const nlohmann::json::exception &)
        {
            // skip invalid files
        }
    }

    return tasks;
}

void Storage::DeleteTask(std::string_view id)
{
    auto task_path = GetTaskPath(id);
    if (std::filesystem::exists(task_path))
        std::filesystem::remove(task_path);

    auto log_path = GetLogPath(id);
    if (std::filesystem::exists(log_path))
        std::filesystem::remove(log_path);

    auto lock_path = GetLockPath(id);
    if (std::filesystem::exists(lock_path))
        std::filesystem::remove(lock_path);
}

void Storage::UpdateTaskStatus(std::string_view id, TaskStatus status, std::string_view error_msg)
{
    auto task = LoadTask(id);
    if (!task)
    {
        fmt::println(stderr, "Warning: Failed to load task {} for status update", id);
        return;
    }

    // ignore status update if task is already in a final state
    if (IsFinalStatus(task->status))
        return;

    task->status = status;
    if (!error_msg.empty())
        task->error_message = error_msg;

    SaveTask(*task);
}

TaskStatus Storage::ResolveTaskStatus(const Task &task)
{
    if (task.status == TaskStatus::Running || task.status == TaskStatus::Pending)
    {
        if (task.daemon_pid <= 0)
            return TaskStatus::Failed;

        if (!IsDaemonLocked(task.id))
            return TaskStatus::Failed;
    }
    return task.status;
}

int Storage::CleanFinishedTasks()
{
    if (!std::filesystem::exists(tasks_dir_))
        return 0;

    std::vector<std::string> to_delete;

    for (const auto &entry : std::filesystem::directory_iterator(tasks_dir_))
    {
        if (entry.path().extension() != ".json")
            continue;

        auto id = entry.path().stem().string();
        auto task = LoadTask(id);

        if (!task)
        {
            // corrupt JSON - delete zombie files
            to_delete.push_back(id);
            continue;
        }

        auto status = ResolveTaskStatus(*task);
        if (IsFinalStatus(status))
            to_delete.push_back(id);
    }

    for (const auto &id : to_delete)
        DeleteTask(id);

    return static_cast<int>(to_delete.size());
}

std::filesystem::path Storage::GetTaskPath(std::string_view id)
{
    return tasks_dir_ / fmt::format("{}.json", id);
}

std::filesystem::path Storage::GetLogPath(std::string_view id)
{
    return logs_dir_ / fmt::format("{}.log", id);
}

std::filesystem::path Storage::GetLockPath(std::string_view id)
{
    return locks_dir_ / fmt::format("{}.lock", id);
}

int Storage::AcquireDaemonLock(std::string_view id)
{
    auto path = GetLockPath(id);
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;

    // acquire exclusive lock and hold the lock until fd is closed (process exits)
    if (flock(fd, LOCK_EX | LOCK_NB) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

bool Storage::IsDaemonLocked(std::string_view id)
{
    auto path = GetLockPath(id);
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    // daemon is alive
    if (flock(fd, LOCK_SH | LOCK_NB) < 0)
    {
        bool is_locked = (errno == EWOULDBLOCK || errno == EAGAIN);
        close(fd);
        return is_locked;
    }

    // daemon is not running (dead or never started)
    flock(fd, LOCK_UN);
    close(fd);
    return false;
}

} // namespace later
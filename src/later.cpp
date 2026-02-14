#include "daemon.h"
#include "executor.h"
#include "storage.h"
#include "task.h"
#include "time_parser.h"
#include "utils.h"

#include "3rdparty/CLI11.hpp"
#include "3rdparty/expected.hpp"
#include "3rdparty/fmt/base.h"

#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

int ListTasks(bool verbose = false)
{
    later::Storage storage;
    auto tasks = storage.ListTasks();

    if (tasks.empty())
    {
        fmt::println("No tasks found");
        return 0;
    }

    std::sort(tasks.begin(), tasks.end(), later::TaskSorter);

    if (verbose)
        fmt::println("{:<3} {:<10} {:<20} {:<20} {:<5} {}", "#", "Status", "Created at", "Execute at",
                     "Cmds", "ID");
    else
        fmt::println("{:<3} {:<10} {:<20} {:<20} {}", "#", "Status", "Created at", "Execute at",
                     "Cmds");
    for (size_t i = 0; i < tasks.size(); ++i)
    {
        auto status = storage.ResolveTaskStatus(tasks[i]);
        if (verbose)
            fmt::println("{:<3} {:<19} {:<20} {:<20} {:<5} {}", i + 1,
                         later::TaskStatusToString(status, true),
                         later::FormatTime(tasks[i].created_at), later::FormatTime(tasks[i].execute_at),
                         tasks[i].commands.size(), tasks[i].id);
        else
            fmt::println("{:<3} {:<19} {:<20} {:<20} {}", i + 1,
                         later::TaskStatusToString(status, true),
                         later::FormatTime(tasks[i].created_at), later::FormatTime(tasks[i].execute_at),
                         tasks[i].commands.size());
    }

    return 0;
}

int ShowTask(const std::string &input_id)
{
    auto id_opt = later::ResolveTaskId(input_id);
    if (!id_opt)
    {
        fmt::println(stderr, "Error: Task not found or ambiguous '{}'", input_id);
        return 1;
    }
    std::string id = *id_opt;

    later::Storage storage;
    auto task = storage.LoadTask(id);
    if (!task)
    {
        fmt::println(stderr, "Error: Task not found '{}': {}", id, task.error());
        return 1;
    }

    auto status = storage.ResolveTaskStatus(*task);

    auto now = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(task->execute_at - now);

    fmt::println("Task: {}", task->id);
    fmt::println("Status:      {}", later::TaskStatusToString(status, true));
    fmt::println("Execute at:  {} ({})", later::FormatTime(task->execute_at),
                 later::FormatDuration(duration));
    fmt::println("Working dir: {}", task->cwd);
    fmt::println("Commands:");
    for (size_t i = 0; i < task->commands.size(); ++i)
        fmt::println("  {}. {}", i + 1, task->commands[i]);
    if (!task->error_message.empty())
        fmt::println("Error: {}", task->error_message);

    return 0;
}

int CancelTask(const std::string &input_id)
{
    auto id_opt = later::ResolveTaskId(input_id);
    if (!id_opt)
    {
        fmt::println(stderr, "Error: Task not found or ambiguous '{}'", input_id);
        return 1;
    }
    std::string id = *id_opt;

    later::Storage storage;
    auto task = storage.LoadTask(id);
    if (!task)
    {
        fmt::println(stderr, "Error: Task not found '{}': {}", id, task.error());
        return 1;
    }

    auto status = storage.ResolveTaskStatus(*task);
    if (status != later::TaskStatus::Pending && status != later::TaskStatus::Running)
    {
        fmt::println("Task {} is already {} ", id, later::TaskStatusToString(status));
        return 0;
    }

    // check if daemon is alive using lock file
    if (!storage.IsDaemonLocked(id))
    {
        fmt::println("Daemon process for task {} is not running", id);
        // try to update status (ignored if task is already in a final state)
        storage.UpdateTaskStatus(id, later::TaskStatus::Failed,
                                 fmt::format("Task {} exited unexpectedly", id));
        return 0;
    }

    // try to kill the process group
    if (task->daemon_pid > 0)
    {
        if (kill(-task->daemon_pid, SIGTERM) == -1)
        {
            if (errno == ESRCH)
                fmt::println("Daemon process {} already exited", task->daemon_pid);
            else if (errno == EPERM)
                fmt::println(stderr, "Warning: No permission to kill process group {}",
                             task->daemon_pid);
            else
                fmt::println(stderr, "Warning: Failed to kill process group {}: {}",
                             task->daemon_pid, std::strerror(errno));
        }
    }

    // update status to Cancelled
    storage.UpdateTaskStatus(id, later::TaskStatus::Cancelled);

    // append cancellation note to task log
    std::ofstream file(storage.GetLogPath(id), std::ios::app);
    if (file)
        file << "Task cancelled by user.\n";
    else
        fmt::println(stderr, "Warning: Failed to write cancellation note to log file");

    fmt::println("Task {} cancelled", id);
    return 0;
}

int ShowLogs(const std::string &input_id)
{
    auto id_opt = later::ResolveTaskId(input_id);
    if (!id_opt)
    {
        fmt::println(stderr, "Error: Task not found or ambiguous '{}'", input_id);
        return 1;
    }
    std::string id = *id_opt;

    later::Storage storage;
    auto log_path = storage.GetLogPath(id);

    if (!std::filesystem::exists(log_path))
    {
        fmt::println(stderr, "Error: Log file not found for task '{}'", id);
        return 1;
    }

    std::ifstream file(log_path);
    if (!file)
    {
        fmt::println(stderr, "Error: Failed to open log file");
        return 1;
    }

    std::string line;
    while (std::getline(file, line))
        fmt::println("{}", line);

    return 0;
}

int CleanTasks()
{
    later::Storage storage;
    int count = storage.CleanFinishedTasks();
    fmt::println("Cleaned {} task(s)", count);
    return 0;
}

int CreateTask(std::string_view time_str, bool dry_run)
{
    // parse time
    auto time_result = later::ParseTime(time_str);
    if (!time_result)
    {
        fmt::println(stderr, "Error: {}", time_result.error());
        return 1;
    }

    auto execute_at = time_result.value();
    auto now = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(execute_at - now);

    // current working directory
    std::string cwd = std::filesystem::current_path().string();

    fmt::println("Current time: {}", later::FormatTime(now));
    fmt::println("Execute at:   {} ({})", later::FormatTime(execute_at),
                 later::FormatDuration(duration));
    fmt::println("Working dir:  {}", cwd);

    auto commands = later::ReadCommands();
    if (commands.empty())
    {
        fmt::println(stderr, "Error: No commands provided");
        return 1;
    }

    // dry run mode - just show what would happen
    if (dry_run)
    {
        fmt::println("[Dry Run] Task preview:");
        fmt::println("  Commands:");
        for (size_t i = 0; i < commands.size(); ++i)
            fmt::println("    {}. {}", i + 1, commands[i]);
        fmt::println("Task will NOT be created (dry-run mode)");
        return 0;
    }

    // create task
    later::Storage storage;
    later::Task task;
    task.id = later::GenerateTaskId();
    task.cwd = cwd;
    task.commands = commands;
    task.created_at = now;
    task.execute_at = execute_at;
    task.status = later::TaskStatus::Pending;

    storage.SaveTask(task);

    // fork
    pid_t pid = fork();
    if (pid < 0)
    {
        fmt::println(stderr, "Error: Failed to fork");
        storage.DeleteTask(task.id);
        return 1;
    }

    if (pid > 0)
    {
        // parent process
        fmt::println("Task created: {}", task.id);
        return 0;
    }

    // daemonize
    auto daemon_result = later::Daemonize(task.id);
    if (!daemon_result)
    {
        storage.UpdateTaskStatus(task.id, later::TaskStatus::Failed, daemon_result.error());
        _exit(1);
    }

    // update pid after daemonize
    task.daemon_pid = getpid();

    // create process group
    setpgid(0, 0);

    storage.SaveTask(task);

    // initial log info
    fmt::println("Task {} scheduled", task.id);
    std::fflush(stdout);

    // sleep until execution time
    std::this_thread::sleep_until(execute_at);
    // small delay for wake from sleep_until
    std::this_thread::sleep_for(std::chrono::seconds(3));

    storage.UpdateTaskStatus(task.id, later::TaskStatus::Running);

    auto result = later::ExecuteCommands(task);
    if (!result)
    {
        storage.UpdateTaskStatus(task.id, later::TaskStatus::Failed, result.error());
        _exit(1);
    }

    int exit_code = result.value();
    if (exit_code == 0)
        storage.UpdateTaskStatus(task.id, later::TaskStatus::Completed);
    else
        storage.UpdateTaskStatus(task.id, later::TaskStatus::Failed,
                                 fmt::format("Exit code: {}", exit_code));

    _exit(exit_code == 0 ? 0 : 1);
}

int DeleteTask(const std::string &input_id)
{
    auto id_opt = later::ResolveTaskId(input_id);
    if (!id_opt)
    {
        fmt::println(stderr, "Error: Task not found or ambiguous '{}'", input_id);
        return 1;
    }
    std::string id = *id_opt;

    later::Storage storage;
    auto task = storage.LoadTask(id);
    if (!task)
    {
        fmt::println(stderr, "Error: Task not found '{}': {}", id, task.error());
        return 1;
    }

    auto status = storage.ResolveTaskStatus(*task);
    if (status == later::TaskStatus::Pending || status == later::TaskStatus::Running)
    {
        fmt::println(stderr, "Error: Task {} is still {}, cancel it first", id,
                     later::TaskStatusToString(status));
        return 1;
    }

    storage.DeleteTask(id);
    fmt::println("Task {} deleted", id);
    return 0;
}

int main(int argc, char *argv[])
{
    CLI::App app{"later - Schedule commands for later execution"};

    std::string time_str;
    std::string cancel_id;
    std::string delete_id;
    std::string logs_id;
    std::string show_id;
    bool list_flag = false;
    bool clean_flag = false;
    bool verbose_flag = false;
    bool dry_run = false;

    app.add_option("time", time_str,
                   "Time to execute (e.g., 17:30, +30m, +2h, 2024-01-28T17:30:00)");
    app.add_flag("-l,--list", list_flag, "List all tasks");
    app.add_option("-s,--show", show_id, "Show details of a task");
    app.add_option("-c,--cancel", cancel_id, "Cancel a task by ID");
    app.add_option("-d,--delete", delete_id, "Delete a single task");
    app.add_option("-L,--logs", logs_id, "Show logs for a task");
    app.add_flag("-C,--clean", clean_flag, "Clean all finished tasks");
    app.add_flag("-v,--verbose", verbose_flag, "Show detailed output");
    app.add_flag("-n,--dry-run", dry_run, "Preview task without creating it");

    CLI11_PARSE(app, argc, argv);

    try
    {
        if (list_flag)
            return ListTasks(verbose_flag);
        if (!show_id.empty())
            return ShowTask(show_id);
        if (!cancel_id.empty())
            return CancelTask(cancel_id);
        if (!delete_id.empty())
            return DeleteTask(delete_id);
        if (!logs_id.empty())
            return ShowLogs(logs_id);
        if (clean_flag)
            return CleanTasks();
        if (!time_str.empty())
            return CreateTask(time_str, dry_run);

        // show help if no arguments
        fmt::print("{}", app.help());
        return 0;
    }
    catch (const std::exception &e)
    {
        fmt::println(stderr, "Error: {}", e.what());
        return 1;
    }
}
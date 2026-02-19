#include "utils.h"

#include "storage.h"
#include "task.h"
#include "time_parser.h"

#include "3rdparty/fmt/base.h"
#include "3rdparty/linenoise/linenoise.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>

namespace later
{

bool TaskSorter(const Task &a, const Task &b)
{
    return a.created_at < b.created_at;
}

void PrintTaskInfo(const Task &task)
{
    Storage storage;
    auto status = storage.ResolveTaskStatus(task);

    fmt::println("  ID:          {}", task.id);
    fmt::println("  Status:      {}", TaskStatusToString(status, true));
    fmt::println("  Execute at:  {}", FormatTime(task.execute_at));
    fmt::println("  Working dir: {}", task.cwd);
    fmt::println("  Commands:    {}", task.commands.size());
    if (!task.error_message.empty())
        fmt::println("  Error: {}", task.error_message);

    fmt::println("");
}

std::optional<std::string> ResolveTaskId(const std::string &id)
{
    Storage storage;
    auto tasks = storage.ListTasks();
    std::sort(tasks.begin(), tasks.end(), TaskSorter);

    // try as index (1-based)
    if (std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); }))
    {
        try
        {
            int index = std::stoi(id);
            if (index >= 1 && index <= static_cast<int>(tasks.size()))
                return tasks[index - 1].id;
        }
        catch (...)
        {
        }
    }

    // try as prefix match
    std::string matched_id;
    int matches = 0;
    for (const auto &task : tasks)
    {
        if (task.id == id)
            return task.id;
        if (task.id.starts_with(id))
        {
            matched_id = task.id;
            ++matches;
        }
    }

    if (matches == 1)
        return matched_id;
    return std::nullopt;
}

std::vector<std::string> ReadCommands()
{
    std::vector<std::string> commands;

    while (true)
    {
        char *line = linenoise("later> ");
        if (!line)
            break;
        if (line[0] == '\0')
        {
            linenoiseFree(line);
            break;
        }
        linenoiseHistoryAdd(line);
        commands.emplace_back(line);
        linenoiseFree(line);
    }

    return commands;
}

std::string GenerateTaskId()
{
    auto now = std::chrono::system_clock::now();
    auto timestamp =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return fmt::format("{}_{}", timestamp, getpid());
}

} // namespace later
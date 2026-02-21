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
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace later
{

namespace
{

std::string UnescapePath(const std::string &path)
{
    std::string result;
    for (size_t i = 0; i < path.length(); ++i)
    {
        if (path[i] == '\\' && i + 1 < path.length() && path[i + 1] == ' ')
        {
            result += ' ';
            ++i;
        }
        else
        {
            result += path[i];
        }
    }
    return result;
}

std::string EscapePath(const std::string &path)
{
    std::string result;
    for (char c : path)
    {
        if (c == ' ')
            result += "\\ ";
        else
            result += c;
    }
    return result;
}

void FileCompletion(const char *buf, linenoiseCompletions *lc)
{
    std::string line(buf);

    // find the start of the current token being typed
    size_t token_start = 0;
    for (size_t i = 0; i < line.length(); ++i)
    {
        if (line[i] == ' ' && (i == 0 || line[i - 1] != '\\'))
            token_start = i + 1;
    }

    std::string context = line.substr(0, token_start);
    std::string token = line.substr(token_start);

    // unescape token to get the real path
    std::string unescaped_token = UnescapePath(token);

    // split into directory and file prefix
    std::string dir_path = ".";
    std::string file_prefix = unescaped_token;

    size_t last_slash = unescaped_token.find_last_of('/');
    if (last_slash != std::string::npos)
    {
        dir_path = unescaped_token.substr(0, last_slash == 0 ? 1 : last_slash);
        file_prefix = unescaped_token.substr(last_slash + 1);
    }
    else if (unescaped_token == "~")
    {
        dir_path = "~";
        file_prefix = "";
    }

    std::string search_dir = dir_path;
    if (search_dir == "~" || search_dir.starts_with("~/"))
    {
        const char *home = std::getenv("HOME");
        if (home)
            search_dir.replace(0, 1, home);
    }

    std::vector<std::string> matches;
    std::error_code ec;

    for (const auto &entry : std::filesystem::directory_iterator(search_dir, ec))
    {
        std::string filename = entry.path().filename().string();

        if (filename.starts_with('.') && !file_prefix.starts_with('.'))
            continue;

        // check if filename starts with the prefix
        if (filename.starts_with(file_prefix))
        {
            std::string match = filename;
            if (entry.is_directory(ec))
                match += "/";
            matches.emplace_back(std::move(match));
        }
    }

    std::sort(matches.begin(), matches.end());

    for (const auto &match : matches)
    {
        std::string completion = context;
        if (last_slash != std::string::npos)
            completion += token.substr(0, token.find_last_of('/') + 1);
        else if (unescaped_token == "~")
            completion += "~/";

        completion += EscapePath(match);

        if (matches.size() == 1 && completion.back() != '/')
            completion += " ";

        linenoiseAddCompletion(lc, completion.c_str());
    }
}

} // namespace

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

    linenoiseSetCompletionCallback(FileCompletion);

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
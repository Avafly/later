#include "executor.h"

#include "task.h"

#include "3rdparty/expected.hpp"
#include "3rdparty/fmt/base.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace later
{

namespace
{

// execute a single command directly
// return exit code, or tl::unexpected on fork/exec/wait failure
tl::expected<int, std::string> RunCommand(const std::string &cmd, const std::string &cwd = "")
{
    pid_t pid = fork();
    if (pid == -1)
    {
        return tl::unexpected(fmt::format("Failed to fork: {}", std::strerror(errno)));
    }

    // child process
    if (pid == 0)
    {
        if (!cwd.empty())
        {
            if (chdir(cwd.c_str()) == -1)
            {
                fmt::println(stderr, "Failed to chdir to {}: {}", cwd, std::strerror(errno));
                _exit(126);
            }
        }

        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        fmt::println(stderr, "Failed to exec: {}", std::strerror(errno));
        _exit(127);
    }

    // parent process
    int status;
    while (waitpid(pid, &status, 0) == -1)
    {
        if (errno != EINTR)
        {
            return tl::unexpected(
                fmt::format("Failed to wait for child: {}", std::strerror(errno)));
        }
    }

    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status))
    {
        int sig = WTERMSIG(status);
        fmt::println(stderr, "Command killed by signal {}", sig);
        return 128 + sig;
    }

    return tl::unexpected("Unknown wait status");
}

} // namespace

tl::expected<int, std::string> ExecuteCommands(const Task &task)
{
    int exit_code = 0;

    for (size_t i = 0; i < task.commands.size(); ++i)
    {
        const auto &cmd = task.commands[i];

        fmt::println("[{}/{}] Executing: {}", i + 1, task.commands.size(), cmd);
        std::fflush(stdout);

        auto result = RunCommand(cmd, task.cwd);
        if (!result)
            return tl::unexpected(result.error());

        exit_code = result.value();
        if (exit_code != 0)
        {
            fmt::println(stderr, "Command failed with exit code: {}", exit_code);
            std::fflush(stderr);
            return exit_code;
        }
    }

    if (exit_code == 0 && !task.commands.empty())
    {
        fmt::println("All commands completed successfully");
        std::fflush(stdout);
    }

    return exit_code;
}

} // namespace later
#include "daemon.h"

#include "storage.h"

#include "3rdparty/expected.hpp"
#include "3rdparty/fmt/format.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace later
{

tl::expected<void, std::string> Daemonize(std::string_view task_id)
{
    Storage storage;
    auto abs_log_path = std::filesystem::absolute(storage.GetLogPath(task_id));
    auto abs_lock_path = std::filesystem::absolute(storage.GetLockPath(task_id));

    // first fork
    pid_t pid = fork();
    if (pid < 0)
        return tl::unexpected(fmt::format("First fork failed: {}", std::strerror(errno)));
    if (pid > 0)
        // parent process exits
        _exit(0);

    // create new session
    if (setsid() < 0)
        return tl::unexpected(fmt::format("setsid failed: {}", std::strerror(errno)));

    // second fork
    pid = fork();
    if (pid < 0)
        return tl::unexpected(fmt::format("Second fork failed: {}", std::strerror(errno)));
    if (pid > 0)
        // first child exits
        _exit(0);

    // acquire lock
    int lock_fd = storage.AcquireDaemonLock(task_id);
    if (lock_fd < 0)
        return tl::unexpected(fmt::format("Failed to acquire lock for task {}", task_id));

    // chdir to root to avoid holding mount points
    if (chdir("/") < 0)
        return tl::unexpected(fmt::format("chdir failed: {}", std::strerror(errno)));

    umask(0022);

    // redirect stdin to /dev/null
    int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null_fd < 0)
        return tl::unexpected(fmt::format("Failed to open /dev/null: {}", std::strerror(errno)));
    if (dup2(null_fd, STDIN_FILENO) < 0)
    {
        close(null_fd);
        return tl::unexpected(fmt::format("dup2 stdin failed: {}", std::strerror(errno)));
    }
    close(null_fd);

    // redirect stdout and stderr to log file
    int log_fd = open(abs_log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0)
        return tl::unexpected(fmt::format("Failed to open log file {}: {}", abs_log_path.string(),
                                          std::strerror(errno)));
    if (dup2(log_fd, STDOUT_FILENO) < 0)
    {
        close(log_fd);
        return tl::unexpected(fmt::format("dup2 stdout failed: {}", std::strerror(errno)));
    }
    if (dup2(log_fd, STDERR_FILENO) < 0)
    {
        close(log_fd);
        return tl::unexpected(fmt::format("dup2 stderr failed: {}", std::strerror(errno)));
    }
    close(log_fd);

    return {};
}

} // namespace later
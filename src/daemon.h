#ifndef DAEMON_H_
#define DAEMON_H_

#include "3rdparty/expected.hpp"

#include <string>
#include <string_view>

namespace later
{

// daemonize the current process using double-fork
tl::expected<void, std::string> Daemonize(std::string_view task_id);

} // namespace later

#endif // DAEMON_H_
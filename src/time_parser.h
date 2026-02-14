#ifndef LATER_TIME_PARSER_H_
#define LATER_TIME_PARSER_H_

#include "3rdparty/expected.hpp"

#include <chrono>
#include <string>
#include <string_view>

namespace later
{

// Supported formats:
//   "17:30" / "17:30:00"           -> Today at that time
//   "2024-01-28T17:30:00"          -> ISO format
//   "+30m" / "+2h" / "+1h30m"      -> Relative time
tl::expected<std::chrono::system_clock::time_point, std::string> ParseTime(std::string_view input);

// format time_point to readable string
std::string FormatTime(const std::chrono::system_clock::time_point &tp);

// format duration to readable string
std::string FormatDuration(std::chrono::seconds duration);

} // namespace later

#endif // LATER_TIME_PARSER_H_
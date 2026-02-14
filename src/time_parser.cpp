#include "time_parser.h"

#include "3rdparty/expected.hpp"
#include "3rdparty/fmt/chrono.h"
#include "3rdparty/fmt/format.h"

#include <chrono>
#include <ctime>
#include <regex>
#include <string>
#include <string_view>

namespace later
{

namespace
{

// parse relative time: +30m, +2h, +1h30m, +0m
tl::expected<std::chrono::seconds, std::string> ParseRelativeTime(std::string_view input)
{
    if (input.empty() || input.front() != '+')
        return tl::unexpected("Relative time must start with '+'");

    std::string str(input.substr(1));
    static const std::regex pattern(R"(^(?:(\d+)h)?(?:(\d+)m)?(?:(\d+)s)?$)");
    std::smatch match;

    if (!std::regex_match(str, match, pattern) || match[0].str().empty())
        return tl::unexpected(fmt::format("Invalid relative time format: {}", input));

    int hours = 0;
    int minutes = 0;
    int seconds = 0;

    if (match[1].matched)
        hours = std::stoi(match[1].str());
    if (match[2].matched)
        minutes = std::stoi(match[2].str());
    if (match[3].matched)
        seconds = std::stoi(match[3].str());

    return std::chrono::hours(hours) + std::chrono::minutes(minutes) +
           std::chrono::seconds(seconds);
}

// parse time-only format: 17:30, 17:30:00
tl::expected<std::chrono::system_clock::time_point, std::string> ParseTimeOnly(
    std::string_view input)
{
    static const std::regex pattern(R"(^(\d{1,2}):(\d{2})(?::(\d{2}))?$)");
    std::string str(input);
    std::smatch match;

    if (!std::regex_match(str, match, pattern))
        return tl::unexpected(fmt::format("Invalid time format: {}", input));

    int hour = std::stoi(match[1].str());
    int minute = std::stoi(match[2].str());
    int second = match[3].matched ? std::stoi(match[3].str()) : 0;

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
        return tl::unexpected(fmt::format("Invalid time values: {}", input));

    auto now = std::chrono::system_clock::now();
    std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm;
    localtime_r(&now_t, &local_tm);

    local_tm.tm_hour = hour;
    local_tm.tm_min = minute;
    local_tm.tm_sec = second;
    local_tm.tm_isdst = -1;

    std::time_t target_t = std::mktime(&local_tm);
    auto target = std::chrono::system_clock::from_time_t(target_t);

    if (target <= now)
    {
        local_tm.tm_mday += 1;
        local_tm.tm_isdst = -1;

        target_t = std::mktime(&local_tm);
        if (target_t == -1)
        {
            return tl::unexpected(
                fmt::format("Unable to calculate tomorrow's time for: {}", input));
        }
        target = std::chrono::system_clock::from_time_t(target_t);
    }

    return target;
}

// parse ISO format: 2024-01-28T17:30:00
tl::expected<std::chrono::system_clock::time_point, std::string> ParseISOTime(
    std::string_view input)
{
    static const std::regex pattern(R"(^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})$)");
    std::string str(input);
    std::smatch match;

    if (!std::regex_match(str, match, pattern))
        return tl::unexpected(fmt::format("Invalid ISO time format: {}", input));

    std::tm tm = {};
    tm.tm_year = std::stoi(match[1].str()) - 1900;
    tm.tm_mon = std::stoi(match[2].str()) - 1;
    tm.tm_mday = std::stoi(match[3].str());
    tm.tm_hour = std::stoi(match[4].str());
    tm.tm_min = std::stoi(match[5].str());
    tm.tm_sec = std::stoi(match[6].str());
    tm.tm_isdst = -1;

    std::time_t target_t = std::mktime(&tm);
    if (target_t == -1)
        return tl::unexpected(fmt::format("Invalid date/time: {}", input));

    auto target = std::chrono::system_clock::from_time_t(target_t);
    auto now = std::chrono::system_clock::now();

    if (target <= now)
        return tl::unexpected(fmt::format("Time {} has already passed", input));

    return target;
}

} // namespace

tl::expected<std::chrono::system_clock::time_point, std::string> ParseTime(std::string_view input)
{
    if (input.empty())
        return tl::unexpected("Empty time string");

    // relative time
    if (input.front() == '+')
    {
        auto duration = ParseRelativeTime(input);
        if (!duration)
            return tl::unexpected(duration.error());
        return std::chrono::system_clock::now() + duration.value();
    }

    // iso format
    if (input.find('T') != std::string_view::npos)
        return ParseISOTime(input);

    // time only
    return ParseTimeOnly(input);
}

std::string FormatTime(const std::chrono::system_clock::time_point &tp)
{
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm local_tm;
    localtime_r(&t, &local_tm);

    return fmt::format("{:%Y-%m-%d %H:%M:%S}", local_tm);
}

std::string FormatDuration(std::chrono::seconds duration)
{
    auto total_seconds = duration.count();
    if (total_seconds == 0)
        return "0s";

    bool is_negative = total_seconds < 0;
    if (is_negative)
        total_seconds = -total_seconds;

    auto hours = total_seconds / 3600;
    auto minutes = (total_seconds % 3600) / 60;
    auto seconds = total_seconds % 60;

    std::string result;
    if (hours > 0)
        result += fmt::format("{}h ", hours);
    if (minutes > 0)
        result += fmt::format("{}m ", minutes);
    if (seconds > 0)
        result += fmt::format("{}s", seconds);

    if (!result.empty() && result.back() == ' ')
        result.pop_back();

    if (is_negative)
        result += " ago";

    return result;
}

} // namespace later
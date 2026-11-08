#pragma once

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

// The UTC second that starts the name of every file Dusk Studio keeps for
// recovery, "20261108-143005Z", so a plain name sort is the order they were
// made in.
namespace duskstudio::recoverystamp
{
inline constexpr std::size_t kStampLength = 16;

inline std::string utcStamp (std::chrono::system_clock::time_point now)
{
    const auto seconds = (long long) std::chrono::floor<std::chrono::seconds> (now.time_since_epoch()).count();
    const auto days = (seconds >= 0 ? seconds : seconds - 86399) / 86400;
    const auto ofDay = seconds - days * 86400;

    // Howard Hinnant's civil_from_days.
    const auto shifted = days + 719468;
    const auto era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const auto dayOfEra = shifted - era * 146097;
    const auto yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const auto dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const auto monthIndex = (5 * dayOfYear + 2) / 153;
    const auto day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
    const auto month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    const auto year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);

    char stamp[128] {};
    std::snprintf (stamp, sizeof (stamp), "%04lld%02lld%02lld-%02lld%02lld%02lldZ",
                   year, month, day, ofDay / 3600, ofDay / 60 % 60, ofDay % 60);
    return stamp;
}

// Whether name starts with a stamp's shape: eight digits, '-', six digits, 'Z'.
inline bool isStamp (std::string_view name) noexcept
{
    if (name.size() < kStampLength) return false;
    for (std::size_t i = 0; i < kStampLength; ++i)
    {
        const char c = name[i];
        if (i == 8 ? c != '-' : i == kStampLength - 1 ? c != 'Z' : (c < '0' || c > '9')) return false;
    }
    return true;
}

// The second a stamp at the start of name gives, or nothing when it is no
// stamp or names no real date and time.
inline std::optional<std::chrono::system_clock::time_point> timeOfStamp (std::string_view name) noexcept
{
    if (! isStamp (name)) return std::nullopt;
    const auto number = [name] (std::size_t at, std::size_t digits)
    {
        long long value = 0;
        for (std::size_t i = at; i < at + digits; ++i)
            value = value * 10 + (name[i] - '0');
        return value;
    };
    const auto year = number (0, 4), month = number (4, 2), day = number (6, 2);
    const auto hour = number (9, 2), minute = number (11, 2), second = number (13, 2);
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    static constexpr int kDaysIn[] { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12 || day < 1 || day > kDaysIn[month - 1] + (month == 2 && leap ? 1 : 0)
        || hour > 23 || minute > 59 || second > 59)
        return std::nullopt;

    // Howard Hinnant's days_from_civil.
    const auto y = year - (month <= 2 ? 1 : 0);
    const auto era = y / 400;
    const auto yearOfEra = y - era * 400;
    const auto dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const auto dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    const auto days = era * 146097 + dayOfEra - 719468;
    return std::chrono::system_clock::time_point (
        std::chrono::seconds (days * 86400 + hour * 3600 + minute * 60 + second));
}
} // namespace duskstudio::recoverystamp

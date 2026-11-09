#pragma once

#include <ctime>
#include <string>

namespace duskstudio
{
// "2026-11-08 14:30" in the user's time zone.
inline std::string localMinuteText (std::time_t when)
{
    std::tm local {};
   #if defined (_WIN32)
    if (localtime_s (&local, &when) != 0) return {};
   #else
    if (localtime_r (&when, &local) == nullptr) return {};
   #endif
    char buffer[32] {};
    std::strftime (buffer, sizeof buffer, "%Y-%m-%d %H:%M", &local);
    return buffer;
}
} // namespace duskstudio

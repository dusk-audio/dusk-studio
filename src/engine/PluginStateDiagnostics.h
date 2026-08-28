#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace duskstudio::pluginstate
{
inline std::string stateRejectedMessage (std::string_view slotKind, int index,
                                         std::size_t bytes, int auxLane = -1)
{
    std::string message = "[Dusk Studio/session] ";
    message.append (slotKind.data(), slotKind.size());
    if (auxLane >= 0)
        message += " lane " + std::to_string (auxLane + 1)
                 + " slot " + std::to_string (index + 1);
    else
        message += " " + std::to_string (index + 1);
    message += " rejected its saved state (" + std::to_string (bytes)
             + " bytes); slot left offline to preserve the saved state";
    return message;
}

inline std::string restoreFailureLine (std::string_view location,
                                       std::string_view pluginName,
                                       std::string_view format,
                                       std::string_view reason)
{
    std::string line (location);
    line += " - ";
    line.append (pluginName.data(), pluginName.size());
    if (! format.empty())
    {
        line += " [";
        line.append (format.data(), format.size());
        line += "]";
    }
    if (! reason.empty())
    {
        line += ": ";
        line.append (reason.data(), reason.size());
    }
    return line;
}
} // namespace duskstudio::pluginstate

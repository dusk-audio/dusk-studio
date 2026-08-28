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
             + " bytes); running at defaults";
    return message;
}
} // namespace duskstudio::pluginstate

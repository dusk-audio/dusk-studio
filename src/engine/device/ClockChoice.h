#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace duskstudio::device
{
// The entry of a device's rate or block-size list to show as the saved one:
// the value the user chose, or the running one when the device does not list
// the chosen value (a saved setup from another device, a driver that rounds).
// -1 when it lists neither.
template <class Value>
int savedClockEntry (const std::vector<Value>& listed, Value chosen, Value running) noexcept
{
    const auto find = [&listed] (Value wanted)
    {
        for (std::size_t i = 0; i < listed.size(); ++i)
            if (std::abs (static_cast<double> (listed[i]) - static_cast<double> (wanted)) < 0.5)
                return static_cast<int> (i);
        return -1;
    };
    const int saved = find (chosen);
    return saved >= 0 ? saved : find (running);
}
} // namespace duskstudio::device

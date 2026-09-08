#pragma once

#include "MidiBackend.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duskstudio::midi::winmm
{
// Pass the raw snapshot once, before publishing it. Suffixes depend on the
// current enumeration order and can collide with an earlier literal name;
// consumers must reject ambiguous identifiers when opening a port.
inline void finishDeviceEnumeration (std::vector<BackendDeviceInfo>& devices)
{
    for (auto& device : devices)
        if (device.identifier.empty())
            device.identifier = device.name;

    const auto appendNumbers = [&devices] (std::string BackendDeviceInfo::* field)
    {
        for (std::size_t i = 0; i < devices.size(); ++i)
        {
            const auto original = devices[i].*field;
            std::size_t number = 1;
            for (auto j = i + 1; j < devices.size(); ++j)
                if (devices[j].*field == original)
                    devices[j].*field += "-" + std::to_string (++number);
        }
    };
    appendNumbers (&BackendDeviceInfo::name);
    appendNumbers (&BackendDeviceInfo::identifier);
}

class InputClock
{
public:
    explicit InputClock (double startTimeMs) noexcept : startMs (startTimeMs) {}

    // Reset only for a new midiInStart epoch, after fencing old callbacks.
    void reset (double startTimeMs) noexcept { startMs = startTimeMs; }

    // startTimeMs and nowMs use backendClockMs(); one worker owns this clock.
    // The nearest modulo epoch assumes delivery delay/clock drift < 2^31 ms
    // (about 24.9 days). A 32-bit timestamp alone cannot resolve longer delays.
    [[nodiscard]] double toBackendTime (std::uint32_t timestampMs, double nowMs) noexcept
    {
        constexpr double wrapMs = 4294967296.0;
        const auto timestamp = static_cast<double> (timestampMs);
        const auto epoch = std::floor ((nowMs - startMs - timestamp + wrapMs / 2.0) / wrapMs);
        const auto elapsed = std::max (0.0, timestamp + epoch * wrapMs);
        const auto time = startMs + elapsed;
        if (time > nowMs)
        {
            // Preserve the fallback's bounded correction for driver-clock lead.
            if (time > nowMs + 2.0)
                startMs -= 1.0;
            return nowMs;
        }
        return time;
    }

private:
    double startMs;
};
} // namespace duskstudio::midi::winmm

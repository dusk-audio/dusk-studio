#pragma once

#include <initializer_list>

namespace duskstudio
{

constexpr double kFallbackTimelineSampleRate = 48000.0;

// The rate the editors measure timeline time at. Positions are stored in device
// samples, so the running rate comes first; a stopped device reads 0, so then
// the rate the device last ran at, then the one the session was saved at.
inline double timelineSampleRate (double runningRate, double lastDeviceRate, double sessionRate) noexcept
{
    for (const double rate : { runningRate, lastDeviceRate, sessionRate })
        if (rate > 0.0)
            return rate;
    return kFallbackTimelineSampleRate;
}

} // namespace duskstudio

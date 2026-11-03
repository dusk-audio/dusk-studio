#pragma once

#include <cstdint>

namespace duskstudio
{
struct DspReadings
{
    double cpuUsage          = 0.0;
    int    engineOverruns    = 0;
    int    backendXruns      = 0;
    int    realtimeDemotions = 0;
};

enum class DspSeverity { normal, risk, disruption };

// Only the backend reports a stream disruption. An engine overrun is a block
// that finished late, which device buffering can absorb, so it ranks with high
// load and a realtime demotion as a risk.
constexpr DspSeverity dspSeverity (const DspReadings& r) noexcept
{
    if (r.backendXruns > 0)
        return DspSeverity::disruption;
    if (r.engineOverruns > 0 || r.cpuUsage > 0.85 || r.realtimeDemotions > 0)
        return DspSeverity::risk;
    return DspSeverity::normal;
}

inline constexpr std::uint32_t kStatusBarBackground = 0xff141418;

constexpr std::uint32_t dspSeverityColour (DspSeverity s) noexcept
{
    switch (s)
    {
        case DspSeverity::disruption: return 0xffe05050;
        case DspSeverity::risk:       return 0xffe0a050;
        case DspSeverity::normal:     break;
    }
    return 0xffb0b0b8;
}
} // namespace duskstudio

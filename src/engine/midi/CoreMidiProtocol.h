#pragma once

#include "MidiBackend.h"

#include <cstdint>
#include <cwctype>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace duskstudio::midi::coremidi
{
inline std::string identifier (std::int32_t uniqueId)
{
    return "coremidi:" + std::to_string (uniqueId);
}

enum class ExternalIdentifier { Device, Endpoint };

// Legacy identifiers depend on this name-prefix test. Match the fallback's
// per-codepoint, current-locale casing; Unicode folding expands some letters.
inline bool legacyNameStartsWith (std::u16string_view name, std::u16string_view prefix) noexcept
{
    const auto next = [] (std::u16string_view& text)
    {
        char32_t value = text.front();
        text.remove_prefix (1);
        if (value >= 0xd800 && value <= 0xdbff && ! text.empty()
            && text.front() >= 0xdc00 && text.front() <= 0xdfff)
        {
            value = 0x10000 + ((value - 0xd800) << 10) + (text.front() - 0xdc00);
            text.remove_prefix (1);
        }
        return value;
    };
    const auto upper = [] (char32_t value)
    {
        if constexpr (sizeof (std::wint_t) < sizeof (char32_t))
            if (value > std::numeric_limits<std::wint_t>::max()) return value;
        return static_cast<char32_t> (std::towupper (static_cast<std::wint_t> (value)));
    };
    while (! prefix.empty())
    {
        if (name.empty()) return false;
        const auto left = next (name), right = next (prefix);
        if (left != right && upper (left) != upper (right)) return false;
    }
    return true;
}

inline BackendDeviceInfo externalEndpointInfo (const BackendDeviceInfo& endpoint,
                                               const BackendDeviceInfo& device, ExternalIdentifier form)
{
    return { device.name, form == ExternalIdentifier::Device ? device.identifier : endpoint.identifier };
}

inline void appendConnectedInfo (BackendDeviceInfo& result, const BackendDeviceInfo& connected)
{
    if (connected.name.empty() && connected.identifier.empty()) return;
    result.name += (result.name.empty() ? "" : ", ") + connected.name;
    result.identifier += (result.identifier.empty() ? "" : ", ") + connected.identifier;
}

// Older macOS backends used the device UID for a single-entity external
// connection; newer ones retain the endpoint UID. Both forms exist in sessions.
struct EndpointIdentity
{
    std::string identifier;
    std::string deviceLegacyIdentifier;
    std::string endpointLegacyIdentifier;
};

inline std::string migrateIdentifier (const std::vector<EndpointIdentity>& endpoints, const std::string& legacy)
{
    if (legacy.empty()) return {};
    std::string result;
    for (const auto& endpoint : endpoints)
        if (endpoint.deviceLegacyIdentifier == legacy || endpoint.endpointLegacyIdentifier == legacy)
        {
            if (! result.empty()) return {};
            result = endpoint.identifier;
        }
    return result;
}

inline std::string legacyIdentifier (const std::vector<EndpointIdentity>& endpoints,
                                     const std::string& identifier, const std::vector<BackendDeviceInfo>& available)
{
    if (identifier.rfind ("coremidi:", 0) != 0) return {};
    std::string result;
    for (const auto& device : available)
        if (migrateIdentifier (endpoints, device.identifier) == identifier)
        {
            if (! result.empty()) return {};
            result = device.identifier;
        }
    return result;
}

// Capture afresh for every input callback: the two clocks may
// advance differently across sleep. Subtract ticks before converting so a long
// uptime does not lose sub-millisecond timing in floating point.
struct ClockAnchor
{
    std::uint64_t hostTicks;
    double nowMs;
    double millisecondsPerTick;

    double toMilliseconds (std::uint64_t ticks) const noexcept
    {
        if (ticks == 0) return nowMs;
        const double delta = ticks >= hostTicks ? static_cast<double> (ticks - hostTicks)
                                               : -static_cast<double> (hostTicks - ticks);
        return nowMs + delta * millisecondsPerTick;
    }
};

} // namespace duskstudio::midi::coremidi

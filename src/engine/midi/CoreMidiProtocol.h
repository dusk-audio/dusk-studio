#pragma once

#include "MidiBackend.h"
#include "../../foundation/MidiBuffer.h"

#include <array>
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

// One parser per source. Storage is bounded by what the input collector can
// carry; an oversized SysEx is discarded whole, through its terminating F7.
// Messages emit on completion: realtime can precede a fragmented message whose
// first-byte timestamp is older. Never hold clock ticks behind incomplete SysEx.
class PacketDecoder
{
public:
    void reset() noexcept
    {
        size = 0;
        expected = 0;
        runningStatus = 0;
        inSysex = false;
        overflow = false;
    }

    template <class Receiver>
    void push (const std::uint8_t* bytes, int count, double timeMs, Receiver&& receive)
    {
        for (int i = 0; bytes != nullptr && i < count; ++i)
        {
            const auto byte = bytes[i];
            if (byte >= 0xf8)
            {
                receive (&bytes[i], 1, timeMs);
                continue;
            }

            if (inSysex)
            {
                if (byte < 0x80 || byte == 0xf7)
                {
                    if (size < data.size()) data[size++] = byte;
                    else overflow = true;
                    if (byte == 0xf7)
                    {
                        if (! overflow) receive (data.data(), static_cast<int> (size), startMs);
                        reset();
                    }
                    continue;
                }
                reset();
            }

            if (byte >= 0x80)
            {
                size = 0;
                expected = 0;
                runningStatus = byte < 0xf0 ? byte : 0;
                if (byte == 0xf7) continue;
                startMs = timeMs;
                data[size++] = byte;
                if (byte == 0xf0)
                {
                    inSysex = true;
                    continue;
                }
                expected = messageSize (byte);
                if (expected == 1)
                {
                    receive (data.data(), 1, startMs);
                    size = 0;
                }
                continue;
            }

            if (size == 0)
            {
                if (runningStatus == 0) continue;
                data[size++] = runningStatus;
                expected = messageSize (runningStatus);
                startMs = timeMs;
            }
            data[size++] = byte;
            if (size == expected)
            {
                receive (data.data(), static_cast<int> (size), startMs);
                size = 0;
            }
        }
    }

private:
    static std::size_t messageSize (std::uint8_t status) noexcept
    {
        if (status < 0xf0) return (status & 0xe0) == 0xc0 ? 2 : 3;
        if (status == 0xf1 || status == 0xf3) return 2;
        if (status == 0xf2) return 3;
        return 1;
    }

    static constexpr std::size_t kMaxMessageBytes = dusk::kMidiBlockBytes - sizeof (double) - sizeof (std::int32_t);
    std::array<std::uint8_t, kMaxMessageBytes> data {};
    std::size_t size = 0;
    std::size_t expected = 0;
    std::uint8_t runningStatus = 0;
    double startMs = 0.0;
    bool inSysex = false;
    bool overflow = false;
};
} // namespace duskstudio::midi::coremidi

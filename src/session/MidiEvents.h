#pragma once

#include <algorithm>
#include <cstdint>

namespace duskstudio
{
// Off events folded into lengthInTicks - no dangling on/off bookkeeping.
// Negative or zero length = "all-notes-off across region" sentinel.
struct MidiNote
{
    int  channel       = 1;     // 1..16
    int  noteNumber    = 60;    // 0..127
    int  velocity      = 100;   // 1..127 (recorded notes always >= 1)
    std::int64_t startTick     = 0;
    std::int64_t lengthInTicks = 0;

    bool operator== (const MidiNote& o) const noexcept
    {
        return channel == o.channel && noteNumber == o.noteNumber
            && velocity == o.velocity && startTick == o.startTick
            && lengthInTicks == o.lengthInTicks;
    }
    bool operator!= (const MidiNote& o) const noexcept { return ! (*this == o); }
};

struct MidiCc
{
    enum class Kind : int
    {
        ControlChange = 0,
        PitchBend,
        ChannelPressure,
        PolyPressure,
        ProgramChange
    };

    static constexpr int kLanesPerChannel = 259;
    static constexpr int kLaneCount = 16 * kLanesPerChannel;

    int  channel    = 1;
    int  controller = 64;       // sustain pedal
    int  value      = 0;
    std::int64_t atTick = 0;
    Kind kind = Kind::ControlChange;

    int maxValue() const noexcept { return kind == Kind::PitchBend ? 16383 : 127; }

    int laneKey() const noexcept
    {
        int lane = std::clamp (controller, 0, 127);
        switch (kind)
        {
            case Kind::ControlChange: break;
            case Kind::PolyPressure: lane += 128; break;
            case Kind::PitchBend: lane = 256; break;
            case Kind::ChannelPressure: lane = 257; break;
            case Kind::ProgramChange: lane = 258; break;
        }
        return (std::clamp (channel, 1, 16) - 1) * kLanesPerChannel + lane;
    }

    std::uint8_t statusByte() const noexcept
    {
        int status = 0xb0;
        switch (kind)
        {
            case Kind::ControlChange: break;
            case Kind::PitchBend: status = 0xe0; break;
            case Kind::ChannelPressure: status = 0xd0; break;
            case Kind::PolyPressure: status = 0xa0; break;
            case Kind::ProgramChange: status = 0xc0; break;
        }
        return static_cast<std::uint8_t> (status | (std::clamp (channel, 1, 16) - 1));
    }

    std::uint8_t dataByte1() const noexcept
    {
        if (kind == Kind::PitchBend)
            return static_cast<std::uint8_t> (std::clamp (value, 0, 16383) & 127);
        return static_cast<std::uint8_t> (std::clamp (
            kind == Kind::ChannelPressure || kind == Kind::ProgramChange ? value : controller, 0, 127));
    }

    std::uint8_t dataByte2() const noexcept
    {
        return static_cast<std::uint8_t> (kind == Kind::PitchBend
            ? (std::clamp (value, 0, 16383) >> 7) : std::clamp (value, 0, 127));
    }

    int messageSize() const noexcept
    {
        return kind == Kind::ChannelPressure || kind == Kind::ProgramChange ? 2 : 3;
    }

    static bool fromMessage (const std::uint8_t* bytes, int size, std::int64_t tick,
                             MidiCc& result) noexcept
    {
        if (bytes == nullptr || size < 2) return false;
        const int status = bytes[0] & 0xf0;
        Kind type;
        switch (status)
        {
            case 0xb0: type = Kind::ControlChange; break;
            case 0xe0: type = Kind::PitchBend; break;
            case 0xd0: type = Kind::ChannelPressure; break;
            case 0xa0: type = Kind::PolyPressure; break;
            case 0xc0: type = Kind::ProgramChange; break;
            default: return false;
        }
        if (size < (status == 0xc0 || status == 0xd0 ? 2 : 3)) return false;
        result = {};
        result.channel = (bytes[0] & 15) + 1;
        result.atTick = tick;
        result.kind = type;
        result.controller = status == 0xb0 || status == 0xa0 ? (bytes[1] & 127) : 0;
        result.value = status == 0xe0 ? ((bytes[1] & 127) | ((bytes[2] & 127) << 7))
            : (bytes[status == 0xc0 || status == 0xd0 ? 1 : 2] & 127);
        return true;
    }

    bool operator== (const MidiCc& o) const noexcept
    {
        return channel == o.channel && controller == o.controller
            && value == o.value && atTick == o.atTick && kind == o.kind;
    }
    bool operator!= (const MidiCc& o) const noexcept { return ! (*this == o); }
};
} // namespace duskstudio

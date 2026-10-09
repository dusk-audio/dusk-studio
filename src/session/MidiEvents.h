#pragma once

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

// `controller` doubles as message-type discriminator: 0..127 = CC.
// Sentinels for pitch-bend / aftertouch land when piano roll surfaces them.
struct MidiCc
{
    int  channel    = 1;
    int  controller = 64;       // sustain pedal
    int  value      = 0;
    std::int64_t atTick = 0;

    bool operator== (const MidiCc& o) const noexcept
    {
        return channel == o.channel && controller == o.controller
            && value == o.value && atTick == o.atTick;
    }
    bool operator!= (const MidiCc& o) const noexcept { return ! (*this == o); }
};
} // namespace duskstudio

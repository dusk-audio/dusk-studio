#pragma once

#include "../foundation/MidiBuffer.h"

#include <array>
#include <cstdint>

namespace duskstudio::midi
{
// One hanging-note reset: per channel, sustain off (CC 64), all notes off
// (CC 123), then all sound off (CC 120). Which channels hold notes is not
// tracked, so all 16 are swept - the synth ignores the redundant ones in the
// same process pass.
constexpr int kHangingResetMessageCount = 16 * 3;

// Hands each message of the reset, in order, to fn (data, numBytes), which
// returns false to stop. False when it stopped.
template <typename Fn>
inline bool forEachHangingResetMessage (Fn&& fn) noexcept
{
    for (int ch = 1; ch <= 16; ++ch)
    {
        const auto status = (std::uint8_t) (0xB0 | (ch - 1));
        for (const std::uint8_t controller : { (std::uint8_t) 64, (std::uint8_t) 123, (std::uint8_t) 120 })
        {
            const std::array<std::uint8_t, 3> message { status, controller, 0 };
            if (! fn (message.data(), (int) message.size()))
                return false;
        }
    }
    return true;
}

// Appends the whole reset at sampleOffset. Callers reserve the capacity up
// front (see the generated-MIDI budget in AudioEngine), so a false return means
// the block overflowed and the caller must flag it rather than retry. Audio
// thread: allocation-free as long as out was reserveBytes()'d off the RT path.
inline bool emitHangingReset (dusk::MidiBuffer& out, int sampleOffset) noexcept
{
    return forEachHangingResetMessage ([&out, sampleOffset] (const std::uint8_t* data, int numBytes)
                                       { return out.addEvent (data, numBytes, sampleOffset); });
}

// The live MIDI a track takes in one block: its input, the on-screen keyboard
// an armed track auditions besides it, and the channel filter over both. -1 is
// no source; channel 0 passes every channel.
struct LiveMidiRoute
{
    int input    = -1;
    int keyboard = -1;
    int channel  = 0;
};

// True when `now` stops taking something `was` took: a source it no longer
// reads, or a channel its filter now drops. The note-off for a key held there
// is dropped with it, so the track's instrument needs the hanging reset. A
// route that only widens leaves held notes to their own note-offs.
inline bool liveRouteNarrows (const LiveMidiRoute& was, const LiveMidiRoute& now) noexcept
{
    const auto stillRead = [&now] (int source) noexcept
    {
        return source < 0 || source == now.input || source == now.keyboard;
    };
    if (! stillRead (was.input) || ! stillRead (was.keyboard))
        return true;
    const bool readAny = was.input >= 0 || was.keyboard >= 0;
    return readAny && now.channel != 0 && now.channel != was.channel;
}
} // namespace duskstudio::midi

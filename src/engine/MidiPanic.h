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

// Appends the whole reset at sampleOffset. Callers reserve the capacity up
// front (see the generated-MIDI budget in AudioEngine), so a false return means
// the block overflowed and the caller must flag it rather than retry. Audio
// thread: allocation-free as long as out was reserveBytes()'d off the RT path.
inline bool emitHangingReset (dusk::MidiBuffer& out, int sampleOffset) noexcept
{
    for (int ch = 1; ch <= 16; ++ch)
    {
        const auto status = (std::uint8_t) (0xB0 | (ch - 1));
        const std::array<std::uint8_t, 3> sustainOff  { status, 64, 0 };
        const std::array<std::uint8_t, 3> allNotesOff { status, 123, 0 };
        const std::array<std::uint8_t, 3> allSoundOff { status, 120, 0 };
        if (! out.addEvent (sustainOff.data(),  (int) sustainOff.size(),  sampleOffset)
            || ! out.addEvent (allNotesOff.data(), (int) allNotesOff.size(), sampleOffset)
            || ! out.addEvent (allSoundOff.data(), (int) allSoundOff.size(), sampleOffset))
            return false;
    }
    return true;
}
} // namespace duskstudio::midi

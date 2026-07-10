#include <catch2/catch_test_macros.hpp>

#include "engine/MidiTimeCodeEmitter.h"
#include "engine/MidiTimeCodeReceiver.h"

#include <juce_audio_basics/juce_audio_basics.h>

using duskstudio::MidiTimeCodeEmitter;
using duskstudio::MidiTimeCodeReceiver;

namespace
{
constexpr double kSr = 48000.0;
constexpr double kSamplesPerFrame30 = kSr / 30.0;   // 1600

juce::int64 framesAt30 (int hh, int mm, int ss, int ff)
{
    return (((juce::int64) hh * 3600 + mm * 60 + ss) * 30) + ff;
}

// Emit one block at a rising transport edge (forces the full-frame sysex) for a
// playhead at `frames` (30 fps), then return the SMPTE hours field carried by
// that sysex, or -1 if none was emitted. hrByte packs the rate in bits 5..6, so
// hours = byte & 0x1F.
int emittedHours (juce::int64 frames)
{
    MidiTimeCodeEmitter e;
    e.prepare (kSr);
    const auto playhead = (juce::int64) ((double) frames * kSamplesPerFrame30);

    dusk::MidiBuffer buf;
    e.generateBlock (0, 512, playhead, /*isRolling*/ true,
                     MidiTimeCodeReceiver::Fps30, buf);
    for (const auto m : buf)
    {
        const auto& msg = m.getMessage();
        if (msg.getRawDataSize() >= 6 && msg.getRawData()[0] == 0xF0)
            return msg.getRawData()[5] & 0x1F;
    }
    return -1;
}
} // namespace

// SMPTE is a 24-hour clock: the MTC hours field is 0-23. Past 24h the emitter
// must wrap, not overflow the field. (framesToSmpte previously omitted the
// % 24, emitting hours=24+.)
TEST_CASE ("MTC emitter: SMPTE hours wrap at the 24h boundary", "[mtc][smpte][wrap]")
{
    REQUIRE (emittedHours (framesAt30 (23, 59, 59, 29)) == 23);  // still in range
    REQUIRE (emittedHours (framesAt30 (24,  0,  0,  0)) ==  0);  // wraps to 0
    REQUIRE (emittedHours (framesAt30 (25,  0,  0,  0)) ==  1);  // 25h -> 1h
}

TEST_CASE ("MTC round-trip: a 24h playhead decodes back to ~0", "[mtc][smpte][wrap]")
{
    MidiTimeCodeEmitter e;
    MidiTimeCodeReceiver rx;
    e.prepare (kSr);
    rx.prepare (kSr);

    const auto frames24h = framesAt30 (24, 0, 0, 0);
    const auto playhead  = (juce::int64) ((double) frames24h * kSamplesPerFrame30);

    dusk::MidiBuffer buf;
    e.generateBlock (0, 512, playhead, true, MidiTimeCodeReceiver::Fps30, buf);

    // Assert emission, not just receiver state: without a startup full-frame SysEx the
    // receiver stays near its initial zero and the frame check below would pass for the
    // wrong reason (nothing was sent).
    bool hasFullFrame = false;
    for (const auto m : buf)
        if (m.getMessage().getRawDataSize() >= 6 && m.getMessage().getRawData()[0] == 0xF0)
        { hasFullFrame = true; break; }
    REQUIRE (hasFullFrame);

    rx.process (buf, 0, 512);

    // The full-frame sysex carried wrapped 00:00:00:00, so the receiver's
    // absolute frame count reads ~0 rather than 2,592,000.
    REQUIRE (rx.getFrames() <= 2);
}

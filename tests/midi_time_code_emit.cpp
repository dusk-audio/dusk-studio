#include <catch2/catch_test_macros.hpp>

#include "engine/MidiTimeCodeEmitter.h"
#include "engine/MidiTimeCodeReceiver.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace
{
using duskstudio::MidiTimeCodeEmitter;
using duskstudio::MidiTimeCodeReceiver;

// Count specific status bytes in a MidiBuffer.
int countOf (const juce::MidiBuffer& buf, juce::uint8 status)
{
    int n = 0;
    for (const auto m : buf)
        if (m.getMessage().getRawDataSize() >= 1
            && (juce::uint8) m.getMessage().getRawData()[0] == status)
            ++n;
    return n;
}

bool containsSysex (const juce::MidiBuffer& buf)
{
    for (const auto m : buf)
        if (m.getMessage().isSysEx()) return true;
    return false;
}
} // namespace

TEST_CASE ("MidiTimeCodeEmitter: rising edge fires full-frame sysex",
           "[mtc][emitter]")
{
    MidiTimeCodeEmitter e;
    e.prepare (48000.0);

    juce::MidiBuffer buf;
    // 1 second playhead at 30 fps → 30 frames. First block with
    // rolling=true: should emit one F0...F7 sysex + start the QF
    // stream.
    e.generateBlock (/*blockStart*/ 0, /*numSamples*/ 512,
                       /*playheadSamples*/ 48000,
                       /*isRolling*/ true,
                       MidiTimeCodeReceiver::Fps30, buf);
    REQUIRE (containsSysex (buf));
    REQUIRE (countOf (buf, 0xF1) >= 1);   // at least one QF in 512 samples
}

TEST_CASE ("MidiTimeCodeEmitter: stopped transport emits nothing",
           "[mtc][emitter]")
{
    MidiTimeCodeEmitter e;
    e.prepare (48000.0);
    juce::MidiBuffer buf;
    e.generateBlock (0, 512, 0, /*isRolling*/ false,
                       MidiTimeCodeReceiver::Fps30, buf);
    REQUIRE (buf.isEmpty());
}

TEST_CASE ("MidiTimeCodeEmitter: round-trip through receiver recovers playhead",
           "[mtc][emitter][roundtrip]")
{
    using R = MidiTimeCodeReceiver;
    MidiTimeCodeEmitter emit;
    R rx;
    const double sr = 48000.0;
    emit.prepare (sr);
    rx  .prepare (sr);

    // Pick a non-trivial playhead: 1m 23.5s @ 30 fps = (1*60 + 23) *
    // 30 + 15 = 2505 frames @ 30 fps. Samples = 2505 / 30 * sr.
    constexpr int hh = 0, mm = 1, ss = 23, ff = 15;
    const juce::int64 expectedFrames = ((juce::int64) hh * 3600
                                         + mm * 60 + ss) * 30 + ff;
    const auto playheadSamples =
        (juce::int64) ((double) expectedFrames * sr / 30.0);

    // Run several blocks so the emitter pumps a full 8-QF sequence.
    // At 30 fps QF cadence is 4/frame = 120 QF/sec → ~67 ms for 8
    // QFs. 1024-sample block at 48k = 21.3 ms → need ~4 blocks.
    juce::int64 blockStart = 0;
    juce::MidiBuffer out;
    for (int b = 0; b < 8; ++b)
    {
        out.clear();
        // Advance playhead linearly to mimic real transport rolling
        // forward — emitter pre-rolls -2 frames internally, slave
        // decodes +2, net should match the master display.
        const auto ps = playheadSamples
                       + (juce::int64) ((double) b * 1024.0);
        emit.generateBlock (blockStart, 1024, ps, true,
                              R::Fps30, out);
        rx.process (out, blockStart, 1024);
        blockStart += 1024;
    }

    REQUIRE (rx.isRolling());
    REQUIRE (rx.getFrameRate() == R::Fps30);
    // The receiver should be reporting a frame value WITHIN ±2 frames
    // of the most-recent emitter playhead (last block's playhead).
    // The full-frame sysex from the first block snapped to the initial
    // playhead exactly; subsequent QFs track linearly.
    const auto lastPlayheadFrames =
        playheadSamples / (juce::int64) (sr / 30.0)
            + 7 * 1024 / (juce::int64) (sr / 30.0);
    const auto delta = std::llabs (rx.getFrames() - lastPlayheadFrames);
    REQUIRE (delta <= 2);
}

TEST_CASE ("MidiTimeCodeEmitter: frame-rate change re-emits sysex",
           "[mtc][emitter]")
{
    using R = MidiTimeCodeReceiver;
    MidiTimeCodeEmitter e;
    e.prepare (48000.0);
    juce::MidiBuffer buf;

    // First block at 30 fps to set rate.
    e.generateBlock (0, 512, 48000, true, R::Fps30, buf);
    REQUIRE (containsSysex (buf));

    // Second block at 25 fps — rate change should re-emit sysex.
    buf.clear();
    e.generateBlock (512, 512, 48000 + 512, true, R::Fps25, buf);
    REQUIRE (containsSysex (buf));
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/MidiTimeCodeReceiver.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace
{
using duskstudio::MidiTimeCodeReceiver;

// Bridge a juce::MidiBuffer into the dusk::MidiBuffer the receiver takes,
// exactly as AudioEngine does per block.
dusk::MidiBuffer toDusk (const juce::MidiBuffer& j)
{
    dusk::MidiBuffer d;
    for (const auto meta : j)
        d.addEvent (meta.getMessage().getRawData(), meta.getMessage().getRawDataSize(),
                    meta.samplePosition);
    return d;
}

// Build the 8 QF bytes that encode (hh, mm, ss, ff) at rate r.
// Nibble layout matches the receiver (and the emitter):
//   0: ff[3:0]    1: ff[4] | (junk)
//   2: ss[3:0]    3: ss[5:4]
//   4: mm[3:0]    5: mm[5:4]
//   6: hh[3:0]    7: rate[1:0] | hh[4]
// Returns 8 raw F1-data bytes (no status byte — caller builds the
// MidiMessage with status 0xF1 + this byte).
std::array<juce::uint8, 8>
buildQfBytes (int hh, int mm, int ss, int ff,
              MidiTimeCodeReceiver::FrameRate rate)
{
    return {
        (juce::uint8) ((0 << 4) | ( ff       & 0x0F)),
        (juce::uint8) ((1 << 4) | ((ff >> 4) & 0x01)),
        (juce::uint8) ((2 << 4) | ( ss       & 0x0F)),
        (juce::uint8) ((3 << 4) | ((ss >> 4) & 0x03)),
        (juce::uint8) ((4 << 4) | ( mm       & 0x0F)),
        (juce::uint8) ((5 << 4) | ((mm >> 4) & 0x03)),
        (juce::uint8) ((6 << 4) | ( hh       & 0x0F)),
        (juce::uint8) ((7 << 4) | ((((int) rate) << 1) | ((hh >> 4) & 0x01))),
    };
}

// Push a full forward 0..7 QF sequence through the receiver in one
// MidiBuffer. blockStartSample lets the test simulate the engine's
// monotonic sync clock cleanly.
void pushSequence (MidiTimeCodeReceiver& rx,
                   int hh, int mm, int ss, int ff,
                   MidiTimeCodeReceiver::FrameRate rate,
                   juce::int64 blockStartSample = 0,
                   int blockSize = 64)
{
    const auto bytes = buildQfBytes (hh, mm, ss, ff, rate);
    juce::MidiBuffer buf;
    for (int i = 0; i < 8; ++i)
        buf.addEvent (juce::MidiMessage (0xF1, bytes[(size_t) i]), i * 4);
    rx.process (toDusk (buf), blockStartSample, blockSize);
}

// Expected absolute SMPTE frame count (with +2 compensation) for a
// receiver that just consumed a full forward sequence encoded with
// (hh, mm, ss, ff).
juce::int64 expectedFrames (int hh, int mm, int ss, int ff,
                            MidiTimeCodeReceiver::FrameRate rate)
{
    if (rate == MidiTimeCodeReceiver::Fps29_97DF)
    {
        const juce::int64 mins  = hh * 60 + mm;
        const juce::int64 dropped = 2 * (mins - mins / 10);
        return ((juce::int64) hh * 3600 + mm * 60 + ss) * 30 + ff - dropped + 2;
    }
    const int fps = (rate == MidiTimeCodeReceiver::Fps24) ? 24
                  : (rate == MidiTimeCodeReceiver::Fps25) ? 25 : 30;
    return ((juce::int64) hh * 3600 + mm * 60 + ss) * fps + ff + 2;
}
} // namespace

TEST_CASE ("MidiTimeCodeReceiver decodes a forward QF sequence at all 4 frame rates",
           "[mtc][receiver]")
{
    using R = MidiTimeCodeReceiver;
    R rx;
    rx.prepare (48000.0);

    SECTION ("24 fps")
    {
        pushSequence (rx, 1, 23, 45, 12, R::Fps24);
        REQUIRE (rx.isRolling());
        REQUIRE (rx.getFrameRate() == R::Fps24);
        REQUIRE (rx.getFrames() == expectedFrames (1, 23, 45, 12, R::Fps24));
    }
    SECTION ("25 fps")
    {
        pushSequence (rx, 2, 15, 30, 20, R::Fps25);
        REQUIRE (rx.isRolling());
        REQUIRE (rx.getFrameRate() == R::Fps25);
        REQUIRE (rx.getFrames() == expectedFrames (2, 15, 30, 20, R::Fps25));
    }
    SECTION ("29.97 drop-frame")
    {
        // Minute 11: not a 10-multiple, drops 2. Test the drop math.
        pushSequence (rx, 0, 11, 30, 15, R::Fps29_97DF);
        REQUIRE (rx.isRolling());
        REQUIRE (rx.getFrameRate() == R::Fps29_97DF);
        REQUIRE (rx.getFrames() == expectedFrames (0, 11, 30, 15, R::Fps29_97DF));
    }
    SECTION ("30 fps non-drop")
    {
        pushSequence (rx, 3, 0, 0, 5, R::Fps30);
        REQUIRE (rx.isRolling());
        REQUIRE (rx.getFrameRate() == R::Fps30);
        REQUIRE (rx.getFrames() == expectedFrames (3, 0, 0, 5, R::Fps30));
    }
}

TEST_CASE ("MidiTimeCodeReceiver: full-frame sysex emits instant SMPTE without +2",
           "[mtc][receiver]")
{
    using R = MidiTimeCodeReceiver;
    R rx;
    rx.prepare (48000.0);

    // F0 7F 7F 01 01 hh|rate mm ss ff F7. 1h 2m 3s 10f @ 30 fps.
    const int hh = 1, mm = 2, ss = 3, ff = 10;
    const juce::uint8 sysex[10] = {
        0xF0, 0x7F, 0x7F, 0x01, 0x01,
        (juce::uint8) ((((int) R::Fps30) << 5) | hh),
        (juce::uint8) mm,
        (juce::uint8) ss,
        (juce::uint8) ff,
        0xF7
    };
    juce::MidiBuffer buf;
    buf.addEvent (juce::MidiMessage (sysex, 10), 0);
    rx.process (toDusk (buf), /*blockStart*/ 0, /*numSamples*/ 64);

    REQUIRE (rx.isRolling());
    REQUIRE (rx.getFrameRate() == R::Fps30);
    // Sysex skips +2 compensation — value is instantaneous.
    const juce::int64 expected = (juce::int64) hh * 3600 * 30
                                + mm * 60 * 30 + ss * 30 + ff;
    REQUIRE (rx.getFrames() == expected);
}

TEST_CASE ("MidiTimeCodeReceiver: reverse-QF freezes output without stutter",
           "[mtc][receiver]")
{
    using R = MidiTimeCodeReceiver;
    R rx;
    rx.prepare (48000.0);

    // Establish a forward baseline first so currentFrames is non-zero.
    pushSequence (rx, 0, 0, 5, 0, R::Fps30);
    REQUIRE (rx.isRolling());
    const auto baselineFrames = rx.getFrames();

    // Reverse-stream: nibbles 7..1 (NO trailing 0). The receiver should
    // park (reversed=true, rolling=false) on the first decreasing-from-
    // expected nibble. We intentionally omit the trailing nibble 0 —
    // that one would correctly wake the receiver into "fresh forward
    // sequence" mode (master resuming forward playback after the
    // scrub). Per the v1 spec: reverse during a scrub freezes output;
    // returning to forward un-freezes.
    const auto bytes = buildQfBytes (0, 0, 4, 29, R::Fps30);
    juce::MidiBuffer rev;
    for (int i = 7; i >= 1; --i)
        rev.addEvent (juce::MidiMessage (0xF1, bytes[(size_t) i]), (7 - i) * 4);
    rx.process (toDusk (rev), /*blockStart*/ 1000, /*numSamples*/ 64);

    REQUIRE (rx.isReversed());
    REQUIRE_FALSE (rx.isRolling());
    REQUIRE (rx.getFrames() == baselineFrames);  // unchanged
}

TEST_CASE ("MidiTimeCodeReceiver: QF watchdog drops rolling on silence",
           "[mtc][receiver]")
{
    using R = MidiTimeCodeReceiver;
    R rx;
    rx.prepare (48000.0);

    pushSequence (rx, 0, 0, 10, 0, R::Fps30);
    REQUIRE (rx.isRolling());

    // Empty block 1 second later (>500 ms watchdog timeout at 48k).
    juce::MidiBuffer empty;
    rx.process (toDusk (empty), /*blockStart*/ 48000, /*numSamples*/ 64);
    REQUIRE_FALSE (rx.isRolling());
}

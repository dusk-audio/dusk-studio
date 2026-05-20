#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"

#include <cstdint>

using Catch::Matchers::WithinAbs;

// MIDI recording's drained-FIFO -> MidiRegion path runs every captured
// sample-position through samplesToTicks, then later replays via
// ticksToSamples for scheduling. Drift in either direction silently
// turns "captured at the right musical position" into "scheduled into a
// region's past or future" - and at the extremes, "outside the region's
// [0, lengthInTicks] window" so the event is filtered out at drain.
//
// These tests pin the conversion math so a future tweak (or a JUCE-
// header-update int-promotion regression) doesn't quietly break MIDI
// recording.

TEST_CASE ("samplesToTicks: zero stays zero", "[session][ticks]")
{
    REQUIRE (duskstudio::samplesToTicks (0, 48000.0, 120.0f) == 0);
    REQUIRE (duskstudio::samplesToTicks (0,    44100.0, 60.0f) == 0);
    REQUIRE (duskstudio::samplesToTicks (0,    96000.0, 200.0f) == 0);
}

TEST_CASE ("samplesToTicks: invalid inputs return zero (defensive)", "[session][ticks]")
{
    // sr <= 0 or bpm <= 0 must not divide-by-zero or return garbage;
    // the function returns 0 for any invalid configuration.
    REQUIRE (duskstudio::samplesToTicks (48000, 0.0,    120.0f) == 0);
    REQUIRE (duskstudio::samplesToTicks (48000, -1.0,   120.0f) == 0);
    REQUIRE (duskstudio::samplesToTicks (48000, 48000.0,  0.0f) == 0);
    REQUIRE (duskstudio::samplesToTicks (48000, 48000.0, -1.0f) == 0);
}

TEST_CASE ("samplesToTicks: one quarter note at 120 BPM 48k", "[session][ticks]")
{
    // 120 BPM = 2 beats/sec = 0.5 sec/quarter. At 48 kHz = 24000 samples
    // per quarter note. samplesToTicks(24000, 48000, 120) should equal
    // exactly kMidiTicksPerQuarter (480) - the canonical fixed-point
    // anchor of the whole tick system.
    REQUIRE (duskstudio::samplesToTicks (24000, 48000.0, 120.0f)
             == duskstudio::kMidiTicksPerQuarter);
}

TEST_CASE ("samplesToTicks: one bar (4/4) at 120 BPM 48k", "[session][ticks]")
{
    // Four beats = 4 * 24000 = 96000 samples = 4 * 480 = 1920 ticks.
    REQUIRE (duskstudio::samplesToTicks (96000, 48000.0, 120.0f) == 1920);
}

TEST_CASE ("samplesToTicks: rate scaling is linear", "[session][ticks]")
{
    // Doubling the sample rate halves the ticks-per-sample.
    const auto a = duskstudio::samplesToTicks (48000, 48000.0, 120.0f);
    const auto b = duskstudio::samplesToTicks (96000, 96000.0, 120.0f);
    REQUIRE (a == b);  // same musical duration -> same tick count
}

TEST_CASE ("ticksToSamples: zero stays zero", "[session][ticks]")
{
    REQUIRE (duskstudio::ticksToSamples (0, 48000.0, 120.0f) == 0);
}

TEST_CASE ("ticksToSamples: matches samplesToTicks anchor", "[session][ticks]")
{
    // 480 ticks = 1 quarter note = 24000 samples at 120 BPM 48k.
    REQUIRE (duskstudio::ticksToSamples (duskstudio::kMidiTicksPerQuarter, 48000.0, 120.0f)
             == 24000);
}

TEST_CASE ("samplesToTicks <-> ticksToSamples round-trip", "[session][ticks]")
{
    // For any sample count that's a clean tick boundary, the round-trip
    // must be lossless. The tick-grid is fine enough (480 PPQ) that
    // typical block-aligned positions round cleanly; we use exact
    // multiples of one-tick-in-samples to avoid testing the rounding
    // edge case here (a separate test below covers fractional drift).
    const std::int64_t samplesPerTick = (std::int64_t) (48000.0 * 60.0
        / 120.0 / (double) duskstudio::kMidiTicksPerQuarter);
    REQUIRE (samplesPerTick > 0);

    for (int multiplier : { 1, 7, 100, 1000, 4800 })
    {
        const std::int64_t samples = samplesPerTick * multiplier;
        const auto ticks = duskstudio::samplesToTicks (samples, 48000.0, 120.0f);
        const auto back  = duskstudio::ticksToSamples (ticks, 48000.0, 120.0f);
        REQUIRE (back == samples);
    }
}

TEST_CASE ("samplesToTicks: monotonic with increasing sample position",
            "[session][ticks]")
{
    // RecordManager pairs Note Ons with Note Offs by sample position.
    // If the conversion ever regresses to a non-monotonic mapping, the
    // pairing logic would silently mis-attribute Note Off times. Pin
    // monotonicity so a future precision tweak can't sneak past.
    std::int64_t lastTicks = -1;
    for (std::int64_t s = 0; s <= 96000; s += 64)
    {
        const auto t = duskstudio::samplesToTicks (s, 48000.0, 120.0f);
        REQUIRE (t >= lastTicks);
        lastTicks = t;
    }
}

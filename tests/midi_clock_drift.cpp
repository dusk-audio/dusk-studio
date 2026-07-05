#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/MidiClockEmitter.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

using duskstudio::MidiClockEmitter;
using Catch::Matchers::WithinAbs;

namespace
{
struct ClockRun
{
    long long count = 0;
    juce::int64 firstSample = -1;
    juce::int64 lastSample  = -1;
};

// Drive the emitter for `seconds` of audio in fixed blocks, counting F8 ticks
// and recording their absolute sample positions so the caller can measure the
// realised tempo (count) and per-tick spacing (mean interval).
ClockRun runClock (double sr, float bpm, double seconds, int blockSize = 512)
{
    MidiClockEmitter e;
    e.prepare (sr);

    const juce::int64 total = (juce::int64) (sr * seconds);
    ClockRun r;
    juce::MidiBuffer buf;
    for (juce::int64 start = 0; start < total; start += blockSize)
    {
        const int n = (int) juce::jmin ((juce::int64) blockSize, total - start);
        buf.clear();
        e.generateBlock (start, n, bpm, /*isRolling*/ true, buf);
        for (const auto m : buf)
        {
            const auto& msg = m.getMessage();
            if (msg.getRawDataSize() == 1 && msg.getRawData()[0] == 0xF8)
            {
                const juce::int64 abs = start + m.samplePosition;
                if (r.firstSample < 0) r.firstSample = abs;
                r.lastSample = abs;
                ++r.count;
            }
        }
    }
    return r;
}
} // namespace

// 120 BPM @ 48k: samplesPerClock = 48000*60/(120*24) = 1000 exactly, so the
// integer-advance path has ZERO drift. Pin the exact tick count and spacing.
TEST_CASE ("MidiClockEmitter: integer samples-per-clock has no drift", "[midiclock][drift]")
{
    const auto r = runClock (48000.0, 120.0f, 300.0);   // 5 minutes
    REQUIRE (r.count == 14400);                          // 48 ticks/s * 300 s
    const double meanInterval = (double) (r.lastSample - r.firstSample)
                              / (double) (r.count - 1);
    REQUIRE_THAT (meanInterval, WithinAbs (1000.0, 1e-9));
}

// 140 BPM @ 44.1k: samplesPerClock = 787.5 — the worst case for a per-tick
// rounder, which adds +0.5 sample EVERY tick (a systematic 0.06% tempo error;
// slaves drifted ~90 ms over 3 minutes). The phase accumulator rounds only at
// emission, so the long-run mean interval matches the exact fraction and the
// tick count is exact.
TEST_CASE ("MidiClockEmitter: fractional samples-per-clock has no systematic drift",
           "[midiclock][drift]")
{
    const double sr = 44100.0;
    const float  bpm = 140.0f;
    const double seconds = 300.0;
    const auto r = runClock (sr, bpm, seconds);

    const double idealSamplesPerClock = (sr * 60.0) / ((double) bpm * 24.0);  // 787.5
    const double idealTicks = (double) bpm / 60.0 * 24.0 * seconds;           // 16800

    // Mean inter-tick interval within a hundredth of a sample of ideal —
    // per-tick jitter is ±0.5 sample from rounding, but it must not
    // accumulate.
    const double meanInterval = (double) (r.lastSample - r.firstSample)
                              / (double) (r.count - 1);
    REQUIRE (std::abs (meanInterval - idealSamplesPerClock) < 0.01);

    // Realised tick count exact within ±1 (block-edge quantisation).
    REQUIRE (std::abs ((double) r.count - idealTicks) <= 1.0);
}

TEST_CASE ("MidiClockEmitter: transport edges emit Start / Stop", "[midiclock]")
{
    MidiClockEmitter e;
    e.prepare (48000.0);
    juce::MidiBuffer buf;

    auto countStatus = [] (const juce::MidiBuffer& b, juce::uint8 s)
    {
        int n = 0;
        for (const auto m : b)
            if (m.getMessage().getRawDataSize() == 1
                && m.getMessage().getRawData()[0] == s) ++n;
        return n;
    };

    // Rising edge -> exactly one FA (Start).
    e.generateBlock (0, 512, 120.0f, /*isRolling*/ true, buf);
    REQUIRE (countStatus (buf, 0xFA) == 1);
    REQUIRE (countStatus (buf, 0xFC) == 0);

    // Falling edge -> exactly one FC (Stop).
    buf.clear();
    e.generateBlock (512, 512, 120.0f, /*isRolling*/ false, buf);
    REQUIRE (countStatus (buf, 0xFC) == 1);
    REQUIRE (countStatus (buf, 0xFA) == 0);
}

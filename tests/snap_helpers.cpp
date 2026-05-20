#include <catch2/catch_test_macros.hpp>

#include "session/SnapHelpers.h"

namespace
{
constexpr double kSr = 48000.0;
}

TEST_CASE ("beatSamples: returns 0 when bpm is zero", "[SnapHelpers]")
{
    REQUIRE (duskstudio::snap::beatSamples (kSr, 0.0f) == 0);
    REQUIRE (duskstudio::snap::beatSamples (kSr, -10.0f) == 0);
}

TEST_CASE ("beatSamples: returns 0 when sample rate is zero", "[SnapHelpers]")
{
    REQUIRE (duskstudio::snap::beatSamples (0.0, 120.0f) == 0);
    REQUIRE (duskstudio::snap::beatSamples (-1.0, 120.0f) == 0);
}

TEST_CASE ("beatSamples: 120 BPM at 48 kHz = 24000 samples per beat", "[SnapHelpers]")
{
    REQUIRE (duskstudio::snap::beatSamples (48000.0, 120.0f) == 24000);
    REQUIRE (duskstudio::snap::beatSamples (48000.0, 60.0f)  == 48000);
    REQUIRE (duskstudio::snap::beatSamples (44100.0, 120.0f) == 22050);
}

TEST_CASE ("beatOrSecondSamples: falls back to 1 second when bpm <= 0", "[SnapHelpers]")
{
    REQUIRE (duskstudio::snap::beatOrSecondSamples (48000.0, 0.0f)   == 48000);
    REQUIRE (duskstudio::snap::beatOrSecondSamples (44100.0, -1.0f)  == 44100);
    REQUIRE (duskstudio::snap::beatOrSecondSamples (48000.0, 120.0f) == 24000);
}

TEST_CASE ("snapDelta: no-op when step <= 0", "[SnapHelpers]")
{
    REQUIRE (duskstudio::snap::snapDelta ((juce::int64) 12345, 0)  == 12345);
    REQUIRE (duskstudio::snap::snapDelta ((juce::int64) -500,  -1) == -500);
}

TEST_CASE ("snapDelta: rounds positive deltas with half-step bias", "[SnapHelpers]")
{
    constexpr juce::int64 step = 1000;
    // Exactly on a step boundary returns itself.
    REQUIRE (duskstudio::snap::snapDelta ((juce::int64) 1000, step) == 1000);
    REQUIRE (duskstudio::snap::snapDelta ((juce::int64) 0,    step) == 0);
    // Below half-step rounds down.
    REQUIRE (duskstudio::snap::snapDelta ((juce::int64) 499,  step) == 0);
    // At/above half-step rounds up.
    REQUIRE (duskstudio::snap::snapDelta ((juce::int64) 500,  step) == 1000);
    REQUIRE (duskstudio::snap::snapDelta ((juce::int64) 1499, step) == 1000);
    REQUIRE (duskstudio::snap::snapDelta ((juce::int64) 1500, step) == 2000);
}

TEST_CASE ("snapDelta: sign-aware rounding for negative deltas", "[SnapHelpers]")
{
    constexpr juce::int64 step = 1000;
    REQUIRE (duskstudio::snap::snapDelta ((juce::int64) -499,  step) == 0);
    REQUIRE (duskstudio::snap::snapDelta ((juce::int64) -500,  step) == -1000);
    REQUIRE (duskstudio::snap::snapDelta ((juce::int64) -1499, step) == -1000);
    REQUIRE (duskstudio::snap::snapDelta ((juce::int64) -1500, step) == -2000);
}

TEST_CASE ("snapAbsolute: no-op when step <= 0 or input negative", "[SnapHelpers]")
{
    REQUIRE (duskstudio::snap::snapAbsolute ((juce::int64) 12345, 0)    == 12345);
    REQUIRE (duskstudio::snap::snapAbsolute ((juce::int64) -100,  1000) == -100);
}

TEST_CASE ("snapAbsolute: rounds to nearest grid point", "[SnapHelpers]")
{
    constexpr juce::int64 step = 1000;
    REQUIRE (duskstudio::snap::snapAbsolute ((juce::int64) 0,    step) == 0);
    REQUIRE (duskstudio::snap::snapAbsolute ((juce::int64) 499,  step) == 0);
    REQUIRE (duskstudio::snap::snapAbsolute ((juce::int64) 500,  step) == 1000);
    REQUIRE (duskstudio::snap::snapAbsolute ((juce::int64) 1499, step) == 1000);
    REQUIRE (duskstudio::snap::snapAbsolute ((juce::int64) 1500, step) == 2000);
}

TEST_CASE ("stepForResolution: musical resolutions at 120 BPM / 48 kHz",
          "[SnapHelpers]")
{
    using duskstudio::SnapResolution;
    constexpr double kSrFast = 48000.0;
    constexpr float  kBpm    = 120.0f;
    constexpr int    kBpb    = 4;
    // Quarter note = 24000 samples (one beat).
    const auto q = duskstudio::snap::stepForResolution (SnapResolution::Quarter,
                                                    kSrFast, kBpm, kBpb);
    REQUIRE (q == 24000);
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::Bar,    kSrFast, kBpm, kBpb) == q * 4);
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::Half,   kSrFast, kBpm, kBpb) == q * 2);
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::Eighth, kSrFast, kBpm, kBpb) == q / 2);
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::Sixteenth,    kSrFast, kBpm, kBpb) == q / 4);
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::ThirtySecond, kSrFast, kBpm, kBpb) == q / 8);
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::SixtyFourth,  kSrFast, kBpm, kBpb) == q / 16);
}

TEST_CASE ("stepForResolution: triplets are 2/3 of the base", "[SnapHelpers]")
{
    using duskstudio::SnapResolution;
    constexpr double kSrFast = 48000.0;
    constexpr float  kBpm    = 120.0f;
    const auto q = duskstudio::snap::stepForResolution (SnapResolution::Quarter, kSrFast, kBpm, 4);
    const auto qt = duskstudio::snap::stepForResolution (SnapResolution::QuarterTriplet, kSrFast, kBpm, 4);
    // 24000 * 2/3 = 16000.
    REQUIRE (qt == 16000);
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::EighthTriplet, kSrFast, kBpm, 4) == q / 2 * 2 / 3);
}

TEST_CASE ("stepForResolution: dotted resolutions are 3/2 of the base", "[SnapHelpers]")
{
    using duskstudio::SnapResolution;
    constexpr double kSrFast = 48000.0;
    constexpr float  kBpm    = 120.0f;
    // Quarter dotted = 24000 * 1.5 = 36000.
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::QuarterDotted, kSrFast, kBpm, 4) == 36000);
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::EighthDotted,  kSrFast, kBpm, 4) == 18000);
}

TEST_CASE ("stepForResolution: timecode/minsec/cd-frames are tempo-independent",
          "[SnapHelpers]")
{
    using duskstudio::SnapResolution;
    constexpr double kSrFast = 48000.0;
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::Timecode, kSrFast, 0.0f, 0) == 1920);  // 48000/25
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::MinSec,   kSrFast, 0.0f, 0) == 48000);
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::CDFrames, kSrFast, 0.0f, 0) == 640);   // 48000/75
    // bpm = 0 only zeros the musical resolutions.
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::Quarter,  kSrFast, 0.0f, 4) == 0);
}

TEST_CASE ("stepForResolution: Bar respects beatsPerBar", "[SnapHelpers]")
{
    using duskstudio::SnapResolution;
    constexpr double kSrFast = 48000.0;
    constexpr float  kBpm    = 120.0f;
    const auto q = duskstudio::snap::stepForResolution (SnapResolution::Quarter, kSrFast, kBpm, 4);
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::Bar, kSrFast, kBpm, 3) == q * 3);
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::Bar, kSrFast, kBpm, 7) == q * 7);
    // beatsPerBar <= 0 clamps up to 1.
    REQUIRE (duskstudio::snap::stepForResolution (SnapResolution::Bar, kSrFast, kBpm, 0) == q);
}

TEST_CASE ("snap math agrees with the legacy TapeStrip inline expression",
          "[SnapHelpers]")
{
    // Verify byte-identical output vs the pre-refactor inline code at the
    // four TapeStrip snap sites. Reference expression (delta-form):
    //   d = (delta + (delta >= 0 ? step/2 : -step/2)) / step * step
    // and (absolute-form):
    //   s = (sample + step/2) / step * step
    auto legacyDelta = [] (juce::int64 d, juce::int64 step) -> juce::int64
    {
        if (step <= 0) return d;
        return ((d + (d >= 0 ? step / 2 : -step / 2)) / step) * step;
    };
    auto legacyAbs = [] (juce::int64 s, juce::int64 step) -> juce::int64
    {
        if (step <= 0 || s < 0) return s;
        return ((s + step / 2) / step) * step;
    };

    const juce::int64 step120 = duskstudio::snap::beatSamples (48000.0, 120.0f);
    REQUIRE (step120 == 24000);

    for (juce::int64 d : { (juce::int64) -100000, (juce::int64) -24001,
                            (juce::int64) -12000,  (juce::int64) -11999,
                            (juce::int64) 0,        (juce::int64) 11999,
                            (juce::int64) 12000,    (juce::int64) 23999,
                            (juce::int64) 24000,    (juce::int64) 100000 })
    {
        REQUIRE (duskstudio::snap::snapDelta (d, step120) == legacyDelta (d, step120));
    }

    for (juce::int64 s : { (juce::int64) 0, (juce::int64) 11999, (juce::int64) 12000,
                            (juce::int64) 23999, (juce::int64) 24000,
                            (juce::int64) 100000 })
    {
        REQUIRE (duskstudio::snap::snapAbsolute (s, step120) == legacyAbs (s, step120));
    }
}

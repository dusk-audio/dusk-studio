// Engine-level audio-path harness: drive the Metronome through real
// blocks and assert on the output buffer. Establishes the "construct a
// DSP unit, pump blocks, measure the audio" pattern for the engine
// without dragging the donor / device / plugin stack. Metronome is the
// lightest meaningful target (pure click synthesis + beat scheduling,
// no DUSK_PLUGINS_PATH dependency).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/Metronome.h"

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSr    = 48000.0;
constexpr int    kBlock = 512;

float blockPeak (const std::vector<float>& L, const std::vector<float>& R)
{
    float p = 0.0f;
    for (size_t i = 0; i < L.size(); ++i)
        p = std::max (p, std::max (std::abs (L[i]), std::abs (R[i])));
    return p;
}

bool anyNaNorInf (const std::vector<float>& v)
{
    for (float x : v)
        if (! std::isfinite (x)) return true;
    return false;
}

// Drives `numBlocks` contiguous blocks starting at sample 0, returns the
// overall peak across the whole run. Each block sees a fresh zero buffer
// (the metronome mixes its click into the output in place).
float runRolling (duskstudio::Metronome& m, int numBlocks, bool rolling)
{
    std::vector<float> L (kBlock), R (kBlock);
    float peak = 0.0f;
    juce::int64 playhead = 0;
    for (int b = 0; b < numBlocks; ++b)
    {
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        m.process (playhead, rolling, L.data(), R.data(), kBlock);
        REQUIRE_FALSE (anyNaNorInf (L));
        REQUIRE_FALSE (anyNaNorInf (R));
        peak = std::max (peak, blockPeak (L, R));
        playhead += kBlock;
    }
    return peak;
}
}

TEST_CASE ("Metronome: disabled + not rolling stays silent", "[metronome]")
{
    duskstudio::Metronome m;
    m.prepare (kSr);
    m.setEnabled (false);

    // 2 seconds of stopped transport — no clicks, pure silence.
    REQUIRE_THAT (runRolling (m, (int) (kSr * 2.0 / kBlock), /*rolling*/ false),
                  WithinAbs (0.0f, 1.0e-9f));
}

TEST_CASE ("Metronome: enabled + rolling emits clicks", "[metronome]")
{
    duskstudio::Metronome m;
    m.prepare (kSr);
    m.setEnabled (true);
    m.setBpm (120.0f);          // beat every 0.5 s = 24000 samples
    m.setBeatsPerBar (4);
    m.setVolumeDb (-6.0f);

    // 2 s @ 120 BPM = at least 4 beat boundaries → audible clicks.
    const float peak = runRolling (m, (int) (kSr * 2.0 / kBlock), /*rolling*/ true);
    REQUIRE (peak > 0.01f);     // something rendered
    REQUIRE (peak <= 1.0f);     // never clips the bus
}

TEST_CASE ("Metronome: rolling=false triggers no new clicks", "[metronome]")
{
    duskstudio::Metronome m;
    m.prepare (kSr);
    m.setEnabled (true);
    m.setBpm (120.0f);

    // Enabled but transport parked — no NEW clicks should fire. (An
    // in-flight click can ring out, but we start cold at sample 0 so
    // there's nothing in flight.)
    REQUIRE_THAT (runRolling (m, (int) (kSr * 1.0 / kBlock), /*rolling*/ false),
                  WithinAbs (0.0f, 1.0e-9f));
}

TEST_CASE ("Metronome: louder volume yields higher peak", "[metronome]")
{
    auto runAt = [] (float volDb)
    {
        duskstudio::Metronome m;
        m.prepare (kSr);
        m.setEnabled (true);
        m.setBpm (120.0f);
        m.setVolumeDb (volDb);
        return runRolling (m, (int) (kSr * 2.0 / kBlock), /*rolling*/ true);
    };

    const float quiet = runAt (-24.0f);
    const float loud  = runAt (0.0f);
    REQUIRE (loud > quiet);
}

// Engine-level audio-path harness: drive the Metronome through real
// blocks and assert on the output buffer. Establishes the "construct a
// DSP unit, pump blocks, measure the audio" pattern for the engine
// without dragging the donor / device / plugin stack. Metronome is the
// lightest meaningful target (pure click synthesis + beat scheduling,
// no DUSK_PLUGINS_PATH dependency).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/Metronome.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
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
    std::int64_t playhead = 0;
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

std::vector<float> beatBody (int beat)
{
    duskstudio::Metronome metronome;
    metronome.prepare (kSr);
    metronome.setEnabled (true);
    metronome.setBpm (120.0f);
    metronome.setVolumeDb (0.0f);
    std::vector<float> left (241), right (241);
    metronome.process ((std::int64_t) beat * 24000 - 1, true,
                       left.data(), right.data(), (int) left.size());
    return { left.begin() + 1, left.end() };
}

std::vector<float> renderTempoChange (std::int64_t start, std::int64_t end,
                                      std::int64_t change, int blockSize,
                                      bool polyphonic, bool forceEnable)
{
    duskstudio::Metronome metronome;
    metronome.prepare (kSr);
    metronome.setEnabled (! forceEnable);
    metronome.setVolumeDb (0.0f);
    metronome.setPolyphonic (polyphonic);
    std::vector<float> left ((size_t) (end - start)), right (left.size());
    for (auto position = start; position < end; )
    {
        const bool slower = position >= change;
        const auto boundary = slower ? end : std::min (end, change);
        const int count = (int) std::min<std::int64_t> (blockSize, boundary - position);
        metronome.setBpm (slower ? 90.0f : 120.0f);
        const double quarterNote = slower
            ? (double) change / 24000.0 + (double) (position - change) / 32000.0
            : (double) position / 24000.0;
        metronome.process (position, true,
                           left.data() + (position - start), right.data() + (position - start),
                           count, forceEnable, quarterNote);
        position += count;
    }
    float stereoDifference = 0.0f;
    for (size_t i = 0; i < left.size(); ++i)
        stereoDifference = std::max (stereoDifference, std::abs (left[i] - right[i]));
    REQUIRE_THAT (stereoDifference, WithinAbs (0.0f, 1.0e-8f));
    return left;
}

void checkBeatBodies (const std::vector<float>& actual, std::int64_t start,
                       const std::vector<std::pair<std::int64_t, int>>& beats)
{
    std::vector<float> expected (actual.size());
    for (const auto& beat : beats)
    {
        const auto body = beatBody (beat.second);
        const auto offset = beat.first - start;
        REQUIRE (offset >= 0);
        REQUIRE ((size_t) offset + body.size() <= expected.size());
        std::copy (body.begin(), body.end(), expected.begin() + offset);
    }
    float error = 0.0f;
    for (size_t i = 0; i < actual.size(); ++i)
        error = std::max (error, std::abs (actual[i] - expected[i]));
    CHECK_THAT (error, WithinAbs (0.0f, 1.0e-5f));
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

TEST_CASE ("Metronome keeps beat phase and accents through tempo changes", "[metronome][tempo-map]")
{
    std::int64_t change = 96000;
    SECTION ("120 to 90 BPM at bar two") { change = 96000; }
    SECTION ("120 to 90 BPM halfway through a beat") { change = 84000; }
    SECTION ("mapped beat boundaries round to timeline samples") { change = 84001; }
    for (const int blockSize : { 64, 257, 1024 })
        for (const bool polyphonic : { false, true })
        {
            CAPTURE (change, blockSize, polyphonic);
            const auto actual = renderTempoChange (0, 167000, change, blockSize, polyphonic, false);
            if (change == 96000)
                checkBeatBodies (actual, 0, { { 24000, 1 }, { 48000, 2 }, { 72000, 3 },
                                               { 96000, 4 }, { 128000, 5 }, { 160000, 6 } });
            else
                checkBeatBodies (actual, 0, { { 24000, 1 }, { 48000, 2 }, { 72000, 3 },
                                               { 100000, 4 }, { 132000, 5 }, { 164000, 6 } });
        }
}

TEST_CASE ("Metronome keeps forced count-in beats anchored before the tempo map origin", "[metronome][tempo-map][count-in]")
{
    for (const int blockSize : { 64, 257, 1024 })
    {
        CAPTURE (blockSize);
        const auto actual = renderTempoChange (-48000, 115000, 48000, blockSize, false, true);
        checkBeatBodies (actual, -48000, { { -24000, -1 }, { 0, 0 }, { 24000, 1 },
                                            { 48000, 2 }, { 80000, 3 }, { 112000, 4 } });
    }
}

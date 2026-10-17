#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../src/dsp/PhaseAnalysisFeed.h"

#include <array>

using Catch::Matchers::WithinAbs;

TEST_CASE ("Phase feed preserves stereo samples across wrapped spans", "[mastering][phase]")
{
    duskstudio::PhaseAnalysisFeed feed (3);
    feed.prepare (48000.0);
    const std::array<float, 6> left { 1, 2, 3, 4, 5, 6 };
    const std::array<float, 6> right { -1, -2, -3, -4, -5, -6 };
    std::array<float, 8> outL {}, outR {};
    feed.push (left.data(), right.data(), 6);
    auto chunk = feed.read (outL.data(), outR.data(), 4);
    REQUIRE (chunk.samples == 4);
    REQUIRE (chunk.sequence == 0);
    REQUIRE_THAT (chunk.sampleRate, WithinAbs (48000.0, 1.0e-6));
    feed.push (left.data(), right.data(), 4);
    chunk = feed.read (outL.data(), outR.data(), 8);
    REQUIRE (chunk.samples == 6);
    REQUIRE (chunk.sequence == 4);
    const std::array<float, 6> expected { 5, 6, 1, 2, 3, 4 };
    for (int i = 0; i < chunk.samples; ++i)
    {
        REQUIRE_THAT (outL[(size_t) i], WithinAbs (expected[(size_t) i], 1.0e-6));
        REQUIRE_THAT (outR[(size_t) i], WithinAbs (-expected[(size_t) i], 1.0e-6));
    }
    REQUIRE (feed.pendingSamples() == 0);
}

TEST_CASE ("Phase feed drops new overflow samples and exposes their gap", "[mastering][phase]")
{
    duskstudio::PhaseAnalysisFeed feed (3);
    feed.prepare (48000.0);
    const std::array<float, 8> left { 1, 2, 3, 4, 5, 6, 7, 8 };
    const std::array<float, 8> right { -1, -2, -3, -4, -5, -6, -7, -8 };
    std::array<float, 8> outL {}, outR {};
    feed.push (left.data(), right.data(), 8);
    REQUIRE (feed.pendingSamples() == 7);
    auto chunk = feed.read (outL.data(), outR.data(), 4);
    REQUIRE (chunk.samples == 4);
    feed.push (left.data(), right.data(), 4);
    chunk = feed.read (outL.data(), outR.data(), 8);
    REQUIRE (chunk.samples == 3);
    REQUIRE (chunk.sequence == 4);
    REQUIRE_THAT (outL[0], WithinAbs (5.0, 1.0e-6));
    REQUIRE_THAT (outR[2], WithinAbs (-7.0, 1.0e-6));
    chunk = feed.read (outL.data(), outR.data(), 8);
    REQUIRE (chunk.samples == 4);
    REQUIRE (chunk.sequence == 8);
    REQUIRE_THAT (outL[0], WithinAbs (1.0, 1.0e-6));
    REQUIRE_THAT (outR[3], WithinAbs (-4.0, 1.0e-6));
}

TEST_CASE ("Phase feed publishes preparation and discontinuities with samples", "[mastering][phase]")
{
    duskstudio::PhaseAnalysisFeed feed (3);
    const std::array<float, 2> left { 0.25f, 0.5f }, right { -0.75f, -1.0f };
    std::array<float, 8> outL {}, outR {};
    feed.prepare (44100.0);
    feed.push (left.data(), right.data(), 2);
    feed.prepare (96000.0);
    feed.push (left.data(), right.data(), 2);
    const auto old = feed.read (outL.data(), outR.data(), 8);
    REQUIRE (old.samples == 2);
    REQUIRE_THAT (old.sampleRate, WithinAbs (44100.0, 1.0e-6));
    const auto current = feed.read (outL.data(), outR.data(), 8);
    REQUIRE (current.samples == 2);
    REQUIRE (current.sequence == 0);
    REQUIRE (current.generation != old.generation);
    REQUIRE_THAT (current.sampleRate, WithinAbs (96000.0, 1.0e-6));
    feed.markDiscontinuity();
    feed.push (left.data(), right.data(), 2);
    const auto next = feed.read (outL.data(), outR.data(), 8);
    REQUIRE (next.samples == 2);
    REQUIRE (next.sequence == 0);
    REQUIRE (next.generation != current.generation);
}

TEST_CASE ("Phase feed discards stale backlog through its consumer", "[mastering][phase]")
{
    duskstudio::PhaseAnalysisFeed feed (3);
    const std::array<float, 2> left { 0.25f, 0.5f }, right { -0.75f, -1.0f };
    std::array<float, 8> outL {}, outR {};
    feed.push (left.data(), right.data(), 2);
    REQUIRE (feed.pendingSamples() == 0);
    feed.prepare (48000.0);
    feed.push (left.data(), right.data(), 2);
    feed.discardPending();
    REQUIRE (feed.pendingSamples() == 0);
    feed.push (left.data(), right.data(), 2);
    const auto chunk = feed.read (outL.data(), outR.data(), 8);
    REQUIRE (chunk.samples == 2);
    REQUIRE (chunk.sequence == 2);
    feed.push (nullptr, right.data(), 2);
    feed.push (left.data(), right.data(), 0);
    REQUIRE (feed.read (nullptr, outR.data(), 8).samples == 0);
    REQUIRE (feed.read (outL.data(), outR.data(), 0).samples == 0);
    REQUIRE (feed.pendingSamples() == 0);
}

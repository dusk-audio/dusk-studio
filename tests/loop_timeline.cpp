#include <catch2/catch_test_macros.hpp>

#include "engine/LoopTimeline.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace
{
using duskstudio::LoopTimelineSpan;

std::vector<LoopTimelineSpan> collectSpans (std::int64_t blockStart,
                                            int numSamples,
                                            bool loopEnabled,
                                            std::int64_t loopStart,
                                            std::int64_t loopEnd)
{
    std::vector<LoopTimelineSpan> spans;
    duskstudio::forEachLoopTimelineSpan (
        blockStart, numSamples, loopEnabled, loopStart, loopEnd,
        [&spans] (const LoopTimelineSpan& span) { spans.push_back (span); });
    return spans;
}

void requireSpan (const LoopTimelineSpan& span,
                  std::int64_t timelineStart,
                  int bufferOffset,
                  int length,
                  bool wrappedBefore)
{
    REQUIRE (span.timelineStart == timelineStart);
    REQUIRE (span.bufferOffset == bufferOffset);
    REQUIRE (span.length == length);
    REQUIRE (span.wrappedBefore == wrappedBefore);
}
} // namespace

TEST_CASE ("LoopTimeline emits one linear span when looping is disabled or invalid",
           "[loop][timeline]")
{
    const auto disabled = collectSpans (75, 32, false, 100, 200);
    REQUIRE (disabled.size() == 1);
    requireSpan (disabled[0], 75, 0, 32, false);

    const auto empty = collectSpans (75, 32, true, 100, 100);
    REQUIRE (empty.size() == 1);
    requireSpan (empty[0], 75, 0, 32, false);

    const auto inverted = collectSpans (75, 32, true, 200, 100);
    REQUIRE (inverted.size() == 1);
    requireSpan (inverted[0], 75, 0, 32, false);
}

TEST_CASE ("LoopTimeline splits a block exactly at an in-block seam",
           "[loop][timeline]")
{
    const auto endingAtSeam = collectSpans (180, 20, true, 100, 200);
    REQUIRE (endingAtSeam.size() == 1);
    requireSpan (endingAtSeam[0], 180, 0, 20, false);

    const auto spans = collectSpans (180, 40, true, 100, 200);

    REQUIRE (spans.size() == 2);
    requireSpan (spans[0], 180, 0, 20, false);
    requireSpan (spans[1], 100, 20, 20, true);
}

TEST_CASE ("LoopTimeline keeps count-in before loop start linear until loop end",
           "[loop][timeline]")
{
    const auto spans = collectSpans (50, 180, true, 100, 200);

    REQUIRE (spans.size() == 2);
    requireSpan (spans[0], 50, 0, 150, false);
    requireSpan (spans[1], 100, 150, 30, true);
}

TEST_CASE ("LoopTimeline normalizes starts at and beyond loop end",
           "[loop][timeline]")
{
    const auto atEnd = collectSpans (200, 10, true, 100, 200);
    REQUIRE (atEnd.size() == 1);
    requireSpan (atEnd[0], 100, 0, 10, true);

    const auto beyond = collectSpans (455, 10, true, 100, 200);
    REQUIRE (beyond.size() == 1);
    requireSpan (beyond[0], 155, 0, 10, true);

    const auto exactCycles = collectSpans (500, 10, true, 100, 200);
    REQUIRE (exactCycles.size() == 1);
    requireSpan (exactCycles[0], 100, 0, 10, true);

    const auto farBeyond = collectSpans (
        std::numeric_limits<std::int64_t>::max(), 10, true, 100, 200);
    REQUIRE (farBeyond.size() == 1);
    requireSpan (farBeyond[0], 107, 0, 10, true);
}

TEST_CASE ("LoopTimeline emits every wrap when one host block spans many loops",
           "[loop][timeline]")
{
    const auto spans = collectSpans (12, 10, true, 10, 13);

    REQUIRE (spans.size() == 4);
    requireSpan (spans[0], 12, 0, 1, false);
    requireSpan (spans[1], 10, 1, 3, true);
    requireSpan (spans[2], 10, 4, 3, true);
    requireSpan (spans[3], 10, 7, 3, true);
}

TEST_CASE ("LoopTimeline handles negative timeline positions without signed overflow",
           "[loop][timeline]")
{
    const auto crossing = collectSpans (-120, 40, true, -200, -100);
    REQUIRE (crossing.size() == 2);
    requireSpan (crossing[0], -120, 0, 20, false);
    requireSpan (crossing[1], -200, 20, 20, true);

    const auto nearMinimum = collectSpans (
        std::numeric_limits<std::int64_t>::min(), 10, true, -200, -100);
    REQUIRE (nearMinimum.size() == 1);
    requireSpan (nearMinimum[0], std::numeric_limits<std::int64_t>::min(),
                 0, 10, false);
}

TEST_CASE ("LoopTimeline does not call back for an empty block",
           "[loop][timeline]")
{
    int callbacks = 0;
    duskstudio::forEachLoopTimelineSpan (
        180, 0, true, 100, 200,
        [&callbacks] (const LoopTimelineSpan&) { ++callbacks; });

    REQUIRE (callbacks == 0);
}

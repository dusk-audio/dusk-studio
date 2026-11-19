#include <catch2/catch_test_macros.hpp>

#include "engine/CaptureSchedule.h"
#include "engine/LoopTimeline.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{
using duskstudio::CaptureSchedule;
using Span = CaptureSchedule::Span;

std::vector<Span> read (const CaptureSchedule& schedule, std::int64_t from, std::int64_t to)
{
    std::vector<Span> parts;
    schedule.forEachIn (from, to, [&parts] (const Span& part) { parts.push_back (part); });
    return parts;
}

// One captured input sample: when it arrived, the pass it went to and where
// on the timeline it went.
struct Sample
{
    std::int64_t input = 0;
    int pass = 0;
    std::int64_t timeline = 0;
    bool operator== (const Sample& other) const noexcept
    {
        return input == other.input && pass == other.pass && timeline == other.timeline;
    }
};

void expand (const Span& span, std::vector<Sample>& into)
{
    for (int i = 0; i < span.length; ++i)
        into.push_back ({ span.inputStart + i, span.passOrdinal, span.timelineStart + i });
}

struct Gesture
{
    std::int64_t rollFrom = 0;
    std::int64_t loopStart = 0, loopEnd = 0;
    std::int64_t captureStart = 0, captureEnd = 0;
    bool loop = true;
};

// The spans a take keeps of one block, the way the engine decides them: the
// loop's passes cut to the capture range, or a linear take's capture range.
std::vector<Span> blockCapture (const Gesture& g, std::int64_t blockStart, std::int64_t clock,
                                int numSamples, int& pass)
{
    std::vector<Span> spans;
    duskstudio::forEachLoopTimelineSpan (blockStart, numSamples, g.loop, g.loopStart, g.loopEnd,
        [&] (const duskstudio::LoopTimelineSpan& t)
        {
            if (t.wrappedBefore) ++pass;
            const auto start = std::max (t.timelineStart, g.captureStart);
            const auto end = std::min (t.timelineStart + t.length, g.captureEnd);
            if (end <= start) return;
            Span span;
            span.inputStart = clock + t.bufferOffset + (start - t.timelineStart);
            span.length = (int) (end - start);
            span.passOrdinal = g.loop ? pass : 0;
            span.timelineStart = start;
            span.startsPass = g.loop && start == g.captureStart;
            span.endsPass = g.loop && end == g.captureEnd;
            spans.push_back (span);
        });
    return spans;
}

std::int64_t nextBlockStart (const Gesture& g, std::int64_t blockStart, int numSamples)
{
    auto next = blockStart + numSamples;
    if (g.loop && next >= g.loopEnd)
        next = g.loopStart + (next - g.loopEnd) % (g.loopEnd - g.loopStart);
    return next;
}

// Runs a gesture for `total` input samples. The input capture writes each
// block's spans at once; the delayed one reads the window `delay` behind the
// block, as a printed track reads its strip's output. Returns both.
std::pair<std::vector<Sample>, std::vector<Sample>> runGesture (const Gesture& g, int blockSize,
                                                                 int delay, std::int64_t total)
{
    CaptureSchedule schedule;
    std::vector<Sample> direct, delayed;
    std::int64_t blockStart = g.rollFrom, clock = 0;
    int pass = 1;
    while (clock < total)
    {
        const int n = (int) std::min<std::int64_t> (blockSize, total - clock);
        for (const auto& span : blockCapture (g, blockStart, clock, n, pass))
        {
            expand (span, direct);
            schedule.add (span);
        }
        const auto windowStart = clock - delay;
        schedule.forEachIn (std::max<std::int64_t> (0, windowStart), windowStart + n,
            [&] (const Span& part)
            {
                const auto offset = part.inputStart - windowStart;
                REQUIRE (offset >= 0);
                REQUIRE (offset + part.length <= n);
                expand (part, delayed);
            });
        clock += n;
        schedule.forgetBefore (clock - delay);
        blockStart = nextBlockStart (g, blockStart, n);
    }
    return { direct, delayed };
}
} // namespace

TEST_CASE ("CaptureSchedule merges a span that carries straight on and keeps one that does not",
           "[record][print]")
{
    CaptureSchedule schedule;
    schedule.add ({ 0, 100, 0, 5000, false, false });
    schedule.add ({ 100, 50, 0, 5100, false, false });
    REQUIRE (schedule.size() == 1);

    // A seek: the next input sample lands elsewhere on the timeline.
    schedule.add ({ 150, 50, 0, 9000, false, false });
    // A gap in what was kept: outside the punch window for a while.
    schedule.add ({ 300, 20, 0, 9150, false, false });
    REQUIRE (schedule.size() == 3);

    const auto parts = read (schedule, 0, 1000);
    REQUIRE (parts.size() == 3);
    CHECK (parts[0].inputStart == 0);
    CHECK (parts[0].length == 150);
    CHECK (parts[0].timelineStart == 5000);
    CHECK (parts[1].timelineStart == 9000);
    CHECK (parts[2].inputStart == 300);
}

TEST_CASE ("CaptureSchedule keeps a pass seam even when the timeline carries on",
           "[record][print]")
{
    CaptureSchedule schedule;
    // A loop the length of the capture range: pass 1 ends where pass 2 begins.
    schedule.add ({ 0, 64, 1, 1000, true, true });
    schedule.add ({ 64, 64, 2, 1000, true, false });
    schedule.add ({ 128, 10, 2, 1064, false, false });
    REQUIRE (schedule.size() == 2);

    const auto parts = read (schedule, 60, 70);
    REQUIRE (parts.size() == 2);
    CHECK (parts[0].passOrdinal == 1);
    CHECK (parts[0].timelineStart == 1060);
    CHECK (parts[0].length == 4);
    CHECK_FALSE (parts[0].startsPass);
    CHECK (parts[0].endsPass);
    CHECK (parts[1].passOrdinal == 2);
    CHECK (parts[1].timelineStart == 1000);
    CHECK (parts[1].length == 6);
    CHECK (parts[1].startsPass);
    CHECK_FALSE (parts[1].endsPass);

    const auto inside = read (schedule, 70, 80);
    REQUIRE (inside.size() == 1);
    CHECK_FALSE (inside[0].startsPass);
    CHECK_FALSE (inside[0].endsPass);
    CHECK (inside[0].timelineStart == 1006);
}

TEST_CASE ("CaptureSchedule forgets spans behind the horizon and drops the oldest when full",
           "[record][print]")
{
    CaptureSchedule schedule;
    schedule.add ({ 0, 10, 0, 0, false, false });
    schedule.add ({ 20, 10, 0, 20, false, false });
    schedule.forgetBefore (10);
    REQUIRE (schedule.size() == 1);
    CHECK (read (schedule, 0, 100).front().inputStart == 20);
    schedule.forgetBefore (29);
    CHECK (schedule.size() == 1);
    schedule.forgetBefore (30);
    CHECK (schedule.size() == 0);

    for (int i = 0; i < CaptureSchedule::kCapacity + 5; ++i)
        schedule.add ({ (std::int64_t) i * 2, 1, 0, (std::int64_t) i * 2, false, false });
    REQUIRE (schedule.size() == CaptureSchedule::kCapacity);
    CHECK (read (schedule, 0, 1'000'000).front().inputStart == 10);
    schedule.reset();
    CHECK (schedule.size() == 0);
    CHECK (read (schedule, 0, 1'000'000).empty());
}

TEST_CASE ("CaptureSchedule read late keeps what the input capture kept, in its passes and places",
           "[record][print]")
{
    const std::vector<Gesture> gestures {
        // Loop recording from the loop start, the whole loop captured.
        { 1000, 1000, 1400, 1000, 1400, true },
        // Count-in before the loop, then a punch inside it.
        { 700, 1000, 1400, 1100, 1350, true },
        // The shortest loop the recorder takes.
        { 5000, 5000, 5128, 5000, 5128, true },
        // A linear take from a punch-in to a punch-out, rolled from before it.
        { 0, 0, 0, 300, 2500, false },
    };
    for (const auto& gesture : gestures)
        for (const int blockSize : { 1, 37, 256, 1024 })
            for (const int delay : { 0, 1, 73, 96, 399, 400, 401, 2000 })
            {
                CAPTURE (gesture.rollFrom, gesture.captureStart, gesture.loop, blockSize, delay);
                const std::int64_t total = 6000;
                const auto [direct, delayed] = runGesture (gesture, blockSize, delay, total);
                REQUIRE_FALSE (direct.empty());
                std::vector<Sample> expected;
                for (const auto& sample : direct)
                    if (sample.input < total - delay) expected.push_back (sample);
                REQUIRE (delayed.size() == expected.size());
                CHECK (delayed == expected);
            }
}

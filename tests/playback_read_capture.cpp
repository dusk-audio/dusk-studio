#include <catch2/catch_test_macros.hpp>

#include "engine/PlaybackReadCapture.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using duskstudio::PlaybackReadCapture;

namespace
{
constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> fixture (int track, int channel, std::int64_t start, int count)
{
    std::vector<float> result (static_cast<std::size_t> (count));
    const auto regionFrames = std::int64_t { 8 } * kRate;
    for (int i = 0; i < count; ++i)
    {
        const auto frame = start + i;
        result[static_cast<std::size_t> (i)] = static_cast<float> (
            PlaybackReadCapture::fixturePcm16 (track, static_cast<int> (frame / regionFrames),
                                              channel, frame % regionFrames)) / 32768.0f;
    }
    return result;
}

void read (PlaybackReadCapture& capture, int track, std::int64_t start, int count,
           std::int64_t loopStart = -1, std::int64_t loopEnd = -1)
{
    const auto left = fixture (track, 0, start, count);
    const auto right = fixture (track, 1, start, count);
    capture.read (track, start, loopStart, loopEnd, left.data(), right.data(), count);
}

PlaybackReadCapture::Expectations plain()
{
    PlaybackReadCapture::Expectations expected;
    expected.sampleRate = kRate;
    return expected;
}
}

TEST_CASE ("playback fixture codes distinguish track region channel and distant frames", "[engine][playback-capture]")
{
    CHECK (PlaybackReadCapture::fixturePcm16 (0, 0, 0, 0) == 521);
    CHECK (PlaybackReadCapture::fixturePcm16 (0, 0, 1, 0) == -5085);
    CHECK (PlaybackReadCapture::fixturePcm16 (23, 14, 1, 1535999) == -15873);
    CHECK (PlaybackReadCapture::fixturePcm16 (4, 7, 0, 60001) == -12827);
}

TEST_CASE ("playback capture checks every channel across region boundaries", "[engine][playback-capture]")
{
    PlaybackReadCapture capture;
    int tracks = 2;
    SECTION ("two tracks") {}
    SECTION ("the full mixer") { tracks = 24; }
    REQUIRE (capture.prepare (256, 2, (1u << tracks) - 1));
    capture.setCapturing (true);
    const auto start = std::int64_t { 8 } * kRate - 64;
    capture.beginCallback (128);
    for (int track = 0; track < tracks; ++track) read (capture, track, start, 128);
    capture.endCallback();
    capture.setCapturing (false);
    const auto result = capture.analyze (plain());
    CHECK (result.passed());
    CHECK (result.checkedValues == static_cast<std::uint64_t> (128 * 2 * tracks));
    CHECK (result.capture.completeCallbacks == 1);
}

TEST_CASE ("playback capture rejects finite but incorrect disk audio", "[engine][playback-capture]")
{
    PlaybackReadCapture capture;
    REQUIRE (capture.prepare (128, 1, 1));
    auto left = fixture (0, 0, 125, 128);
    auto right = fixture (0, 1, 125, 128);
    SECTION ("silence") { std::fill (left.begin(), left.end(), 0.0f); }
    SECTION ("another track") { left = fixture (1, 0, 125, 128); }
    SECTION ("another region") { left = fixture (0, 0, 8 * kRate + 125, 128); }
    SECTION ("swapped channel") { left = right; }
    SECTION ("old periodic pattern displacement") { left = fixture (0, 0, 60126, 128); }
    SECTION ("one missing sample") { left[75] = 0.0f; }
    SECTION ("non-finite") { left[75] = std::numeric_limits<float>::quiet_NaN(); }
    capture.setCapturing (true);
    capture.beginCallback (128);
    capture.read (0, 125, -1, -1, left.data(), right.data(), 128);
    capture.endCallback();
    capture.setCapturing (false);
    const auto result = capture.analyze (plain());
    CHECK_FALSE (result.passed());
    CHECK (result.mismatchedValues > 0);
}

TEST_CASE ("playback capture rejects omitted duplicated or malformed track taps", "[engine][playback-capture]")
{
    PlaybackReadCapture capture;
    REQUIRE (capture.prepare (128, 1, 3));
    capture.setCapturing (true);
    capture.beginCallback (128);
    SECTION ("missing one track") { read (capture, 0, 0, 128); }
    SECTION ("missing all tracks") {}
    SECTION ("duplicate track")
    {
        read (capture, 0, 0, 128);
        read (capture, 0, 0, 128);
        read (capture, 1, 0, 128);
    }
    SECTION ("different positions in the same callback")
    {
        read (capture, 0, 0, 128);
        read (capture, 1, 1, 128);
    }
    SECTION ("partial block")
    {
        read (capture, 0, 0, 127);
        read (capture, 1, 0, 128);
    }
    SECTION ("absent right channel")
    {
        const auto left = fixture (0, 0, 0, 128);
        capture.read (0, 0, -1, -1, left.data(), nullptr, 128);
        read (capture, 1, 0, 128);
    }
    capture.endCallback();
    capture.setCapturing (false);
    CHECK_FALSE (capture.analyze (plain()).passed());
}

TEST_CASE ("playback capture bounds storage and never accepts a truncated run", "[engine][playback-capture]")
{
    PlaybackReadCapture capture;
    SECTION ("sample capacity") { REQUIRE (capture.prepare (64, 2, 1)); }
    SECTION ("callback capacity") { REQUIRE (capture.prepare (256, 1, 1)); }
    SECTION ("re-prepare releases all old buffer capacity")
    {
        REQUIRE (capture.prepare (4096, 4096, 3));
        const auto largeBytes = capture.allocatedBytes();
        REQUIRE (capture.prepare (64, 1, 1));
        PlaybackReadCapture fresh;
        REQUIRE (fresh.prepare (64, 1, 1));
        CHECK (capture.allocatedBytes() == fresh.allocatedBytes());
        CHECK (capture.allocatedBytes() < largeBytes / 100);
        return;
    }
    capture.setCapturing (true);
    for (int block = 0; block < 2; ++block)
    {
        capture.beginCallback (128);
        read (capture, 0, block * 128, 128);
        capture.endCallback();
    }
    capture.setCapturing (false);
    const auto result = capture.analyze (plain());
    CHECK (result.capture.overflow);
    CHECK_FALSE (result.passed());
    CHECK_FALSE (capture.prepare (std::numeric_limits<std::size_t>::max(), 1, 1));
    CHECK_FALSE (capture.prepare (1, 1, 0));
    CHECK_FALSE (capture.prepare (1, 1, 1u << 24));
}

TEST_CASE ("playback capture checks unaligned loop masks independently", "[engine][playback-capture]")
{
    PlaybackReadCapture capture;
    REQUIRE (capture.prepare (512, 4, 1));
    auto expected = plain();
    expected.loopStart = 2 * kRate + 37;
    expected.loopEnd = 4 * kRate + 113;
    expected.requireLoopWrap = true;
    capture.setCapturing (true);
    const auto start = expected.loopEnd - 75;
    auto left = fixture (0, 0, start, 75);
    auto right = fixture (0, 1, start, 75);
    const auto tailL = fixture (0, 0, expected.loopStart, 181);
    const auto tailR = fixture (0, 1, expected.loopStart, 181);
    left.insert (left.end(), tailL.begin(), tailL.end());
    right.insert (right.end(), tailR.begin(), tailR.end());
    for (int sample = 11; sample < 139; ++sample)
    {
        const auto distance = std::abs (sample - 75);
        const float gain = static_cast<float> (std::pow (std::sin (kPi * distance / 128.0), 2));
        left[static_cast<std::size_t> (sample)] *= gain;
        right[static_cast<std::size_t> (sample)] *= gain;
    }
    bool shouldPass = true;
    SECTION ("masked seam") {}
    SECTION ("unmasked seam") { left[74] = fixture (0, 0, expected.loopEnd - 1, 1)[0]; shouldPass = false; }
    capture.beginCallback (256);
    capture.read (0, start, expected.loopStart, expected.loopEnd, left.data(), right.data(), 256);
    capture.endCallback();
    capture.beginCallback (128);
    read (capture, 0, expected.loopStart + 181, 128, expected.loopStart, expected.loopEnd);
    capture.endCallback();
    capture.setCapturing (false);
    const auto result = capture.analyze (expected);
    CHECK (result.loopWraps == 1);
    CHECK (result.passed() == shouldPass);
}

TEST_CASE ("playback capture requires observed seek commits and rejects other timeline jumps", "[engine][playback-capture]")
{
    PlaybackReadCapture capture;
    REQUIRE (capture.prepare (640, 5, 1));
    auto expected = plain();
    expected.rollingSeeks = { 1000, 2000, 3000 };
    std::vector<std::int64_t> starts { 0, 1000, 2000, 3000 };
    bool shouldPass = true;
    SECTION ("all three committed") {}
    SECTION ("only two committed") { starts.pop_back(); shouldPass = false; }
    SECTION ("an unexpected discontinuity") { starts[2] += 1; shouldPass = false; }
    SECTION ("seeks arrive out of order") { std::swap (starts[1], starts[2]); shouldPass = false; }
    capture.setCapturing (true);
    for (const auto start : starts)
    {
        capture.beginCallback (128);
        read (capture, 0, start, 128);
        capture.endCallback();
    }
    capture.setCapturing (false);
    const auto result = capture.analyze (expected);
    CHECK (result.passed() == shouldPass);
    if (shouldPass) CHECK (result.committedSeeks == 3);
}

TEST_CASE ("playback capture gates whole callbacks and rejects an unfinished callback", "[engine][playback-capture]")
{
    PlaybackReadCapture capture;
    REQUIRE (capture.prepare (256, 2, 1));
    capture.beginCallback (128);
    capture.endCallback();
    CHECK_FALSE (capture.analyze (plain()).passed());
    capture.setCapturing (true);
    capture.beginCallback (128);
    read (capture, 0, 0, 128);
    capture.setCapturing (false);
    SECTION ("a stop request completes the active callback")
    {
        capture.endCallback();
        CHECK (capture.analyze (plain()).passed());
    }
    SECTION ("an omitted end is a protocol error") { CHECK_FALSE (capture.analyze (plain()).passed()); }
}

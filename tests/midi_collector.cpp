#include <catch2/catch_test_macros.hpp>

#include "foundation/MidiBuffer.h"
#include "foundation/MidiCollector.h"

#include <cstdint>
#include <vector>

using dusk::MidiCollector;

namespace
{
// Offsets, in iteration order, that a drain produced.
std::vector<int> offsets (const dusk::MidiBuffer& b)
{
    std::vector<int> v;
    for (const auto meta : b)
        v.push_back (meta.samplePosition);
    return v;
}

// Push a 1-byte message whose payload byte is `tag`, stamped at timeMs.
void add (MidiCollector& c, std::uint8_t tag, double timeMs)
{
    c.addMessage (&tag, 1, timeMs);
}
} // namespace

// The offsets below are hand-computed from JUCE's MidiMessageCollector integer
// arithmetic (right-align / squeeze+window / >1 s trim), so these assert the
// semantics are reproduced exactly, not merely plausibly.

TEST_CASE ("MidiCollector right-aligns a short backlog", "[midi][collector]")
{
    MidiCollector c;
    c.reset (48000.0, 0.0);
    add (c, 1, 0.0);    // sample 0
    add (c, 2, 2.0);    // sample 96
    add (c, 3, 5.0);    // sample 240

    dusk::MidiBuffer out;
    c.removeNextBlock (out, 512, 10.0);   // numSourceSamples 480 -> startSample 32
    REQUIRE (offsets (out) == std::vector<int> { 32, 128, 272 });
}

TEST_CASE ("MidiCollector squeezes a backlog longer than the block", "[midi][collector]")
{
    MidiCollector c;
    c.reset (48000.0, 0.0);
    add (c, 1, 0.0);    // sample 0
    add (c, 2, 10.0);   // sample 480
    add (c, 3, 20.0);   // sample 960

    dusk::MidiBuffer out;
    c.removeNextBlock (out, 512, 20.0);   // numSourceSamples 960, scale 546
    REQUIRE (offsets (out) == std::vector<int> { 0, 255, 511 });
}

TEST_CASE ("MidiCollector windows an extreme backlog at numSamples<<5", "[midi][collector]")
{
    MidiCollector c;
    c.reset (48000.0, 0.0);
    add (c, 1, 50.0);    // sample 2400  - before the window start, skipped
    add (c, 2, 350.0);   // sample 16800
    add (c, 3, 400.0);   // sample 19200

    dusk::MidiBuffer out;
    // numSourceSamples 19200 > 16384 -> startSample 2816, scale 32.
    c.removeNextBlock (out, 512, 400.0);
    REQUIRE (offsets (out) == std::vector<int> { 437, 511 });
}

TEST_CASE ("MidiCollector trims events older than one second", "[midi][collector]")
{
    MidiCollector c;
    c.reset (48000.0, 0.0);
    // Large block so the window bound (numSamples<<5 = 65536) sits above the
    // 1 s trip point (sampleRate = 48000); the trim then bites independently.
    add (c, 1, 100.0);    // sample 4800  - older than 1 s before the newest, dropped
    add (c, 2, 600.0);    // sample 28800
    add (c, 3, 1400.0);   // sample 67200 -> triggers trim floor 19200

    dusk::MidiBuffer out;
    c.removeNextBlock (out, 2048, 1400.0);
    REQUIRE (offsets (out) == std::vector<int> { 848, 2047 });
}

TEST_CASE ("MidiCollector clamps a past timestamp to zero", "[midi][collector]")
{
    MidiCollector c;
    c.reset (48000.0, 100.0);
    add (c, 1, 90.0);     // sample -480 (before last drain) -> offset clamps to 0

    dusk::MidiBuffer out;
    c.removeNextBlock (out, 512, 105.0);
    REQUIRE (offsets (out) == std::vector<int> { 0 });
}

TEST_CASE ("MidiCollector: empty drain still advances the clock", "[midi][collector]")
{
    MidiCollector c;
    c.reset (48000.0, 0.0);

    dusk::MidiBuffer out;
    c.removeNextBlock (out, 512, 10.0);   // nothing pending
    REQUIRE (offsets (out).empty());

    // The next block measures elapsed time from 10 ms, not 0: an event at 12 ms
    // right-aligns against a ~2 ms span (numSourceSamples 96 -> startSample 416).
    add (c, 1, 12.0);     // sample (12-10)*48 = 96
    c.removeNextBlock (out, 512, 12.0);
    REQUIRE (offsets (out) == std::vector<int> { 511 });   // 96 + (512-96) = 512 -> clamp 511
}

TEST_CASE ("MidiCollector: monotone input yields non-decreasing offsets", "[midi][collector]")
{
    MidiCollector c;
    c.reset (48000.0, 0.0);

    const double blockMs = 512.0 / 48000.0 * 1000.0;   // ~10.667 ms per 512-sample block
    for (int k = 0; k < 32; ++k)
    {
        const double prev = k * blockMs;
        const double now  = (k + 1) * blockMs;
        add (c, 1, prev + 0.2 * blockMs);
        add (c, 2, prev + 0.5 * blockMs);
        add (c, 3, prev + 0.9 * blockMs);

        dusk::MidiBuffer out;
        c.removeNextBlock (out, 512, now);
        const auto o = offsets (out);
        REQUIRE (o.size() == 3u);
        for (std::size_t i = 0; i < o.size(); ++i)
        {
            REQUIRE (o[i] >= 0);
            REQUIRE (o[i] <= 511);
            if (i > 0) REQUIRE (o[i] >= o[i - 1]);
        }
    }
}

TEST_CASE ("MidiCollector: full ring drops the overflowing message", "[midi][collector]")
{
    // Tiny ring: one 3-byte record is 15 bytes, so a 16-byte ring holds exactly
    // one pending message and the second push is dropped.
    MidiCollector c (16);
    c.reset (48000.0, 0.0);
    const std::uint8_t a[] = { 0x90, 0x40, 0x7F };
    const std::uint8_t b[] = { 0x80, 0x40, 0x00 };
    REQUIRE (c.addMessage (a, 3, 1.0));
    REQUIRE_FALSE (c.addMessage (b, 3, 2.0));

    dusk::MidiBuffer out;
    c.removeNextBlock (out, 512, 2.0);
    REQUIRE (offsets (out).size() == 1u);   // only the accepted message survives
}

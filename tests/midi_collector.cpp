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

// The engine's per-input block buffer is reserveBytes()-capped, so a burst can
// outrun it. Delivering the head of the block and dropping the tail would let a
// note-on through while its note-off was lost, hanging the note - the whole
// block goes instead, the same policy the MIDI out path applies.
TEST_CASE ("MidiCollector drops the whole block when the destination is capped",
           "[midi][collector]")
{
    const std::uint8_t on[]  = { 0x90, 0x40, 0x7F };
    const std::uint8_t off[] = { 0x80, 0x40, 0x00 };

    // Header (2 ints) + 3 payload bytes = 11 bytes per event, so this cap holds
    // exactly one of the pair.
    dusk::MidiBuffer out;
    out.reserveBytes (11);

    MidiCollector c;
    c.reset (48000.0, 0.0);
    REQUIRE (c.addMessage (on,  3, 1.0));
    REQUIRE (c.addMessage (off, 3, 2.0));
    c.removeNextBlock (out, 512, 3.0);
    REQUIRE (out.isEmpty());

    // The ring was still drained, so the next block starts clean and a pair
    // that fits is delivered in full.
    REQUIRE (c.addMessage (on, 3, 4.0));
    c.removeNextBlock (out, 512, 5.0);
    REQUIRE (offsets (out).size() == 1u);
}

// The squeeze path (backlog longer than the block) adds events through its own
// branch, so the cap has to be honoured there too.
TEST_CASE ("MidiCollector drops a squeezed block whole when the cap is hit",
           "[midi][collector]")
{
    dusk::MidiBuffer out;
    out.reserveBytes (22);   // two 3-byte events

    MidiCollector c;
    c.reset (48000.0, 0.0);
    const std::uint8_t note[] = { 0x90, 0x40, 0x7F };
    REQUIRE (c.addMessage (note, 3, 0.0));
    REQUIRE (c.addMessage (note, 3, 10.0));
    REQUIRE (c.addMessage (note, 3, 20.0));

    // numSourceSamples 960 > 512 - the squeeze branch, as in the offsets test
    // above. The third event overruns the cap and takes the block with it.
    c.removeNextBlock (out, 512, 20.0);
    REQUIRE (out.isEmpty());
}

// A record no cap could ever hold is undeliverable however the block is cut, so
// failing the block over it would silence that input for as long as the device
// keeps sending them. A DX7-class bulk dump is well inside the ring's capacity
// and well past the destination's, which is exactly the case that has to drop
// on its own.
TEST_CASE ("MidiCollector drops an oversized record alone", "[midi][collector]")
{
    std::vector<std::uint8_t> sysex (4104, 0x7F);
    sysex.front() = 0xF0;
    sysex.back()  = 0xF7;

    const std::uint8_t on[]  = { 0x90, 0x40, 0x7F };
    const std::uint8_t off[] = { 0x80, 0x40, 0x00 };

    dusk::MidiBuffer out;
    out.reserveBytes (4096);

    MidiCollector c (16384);
    c.reset (48000.0, 0.0);
    REQUIRE (c.addMessage (on, 3, 1.0));
    REQUIRE (c.addMessage (sysex.data(), (int) sysex.size(), 1.5));
    REQUIRE (c.addMessage (off, 3, 2.0));

    c.removeNextBlock (out, 512, 3.0);

    // The note pair survives intact; only the dump is gone.
    int n = 0;
    for (const auto meta : out)
    {
        REQUIRE (meta.numBytes == 3);
        REQUIRE (meta.data[0] == (n == 0 ? 0x90 : 0x80));
        ++n;
    }
    REQUIRE (n == 2);
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

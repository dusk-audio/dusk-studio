#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "foundation/MidiBuffer.h"
#include "foundation/MidiCollector.h"

#include <array>
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

// Whole-block-or-nothing only holds the hung-note line if the destination can
// take everything the ring can. A destination smaller than the ring turns a
// mid-size backlog into an EMPTY block - every note-off in it lost, for notes
// whose note-ons went out in an earlier block. Both sides come from
// kMidiBlockBytes so the drop can only mean "the ring itself overflowed".
TEST_CASE ("MidiCollector delivers a ring-full backlog into a matching destination",
           "[midi][collector]")
{
    MidiCollector c (dusk::kMidiBlockBytes);
    c.reset (48000.0, 0.0);

    // Measure how many 3-byte messages the ring really holds rather than
    // deriving it from the record header sizes, so this still fills the ring if
    // either header changes. Kept on a pair boundary so the backlog ends on a
    // note-off; 372 is what the old 4 KB destination could take.
    int ringMessages = 0;
    {
        MidiCollector probe (dusk::kMidiBlockBytes);
        const std::uint8_t filler[3] { 0x90, 0x40, 0x7F };
        while (probe.addMessage (filler, 3, 1.0)) ++ringMessages;
    }
    const int numMessages = ringMessages & ~1;
    REQUIRE (numMessages > 372);

    std::vector<std::array<std::uint8_t, 3>> pushed;
    for (int i = 0; i < numMessages; ++i)
    {
        const bool noteOn = (i % 2) == 0;
        const std::array<std::uint8_t, 3> msg { (std::uint8_t) (noteOn ? 0x90 : 0x80),
                                                (std::uint8_t) (36 + (i / 2) % 60),
                                                (std::uint8_t) (noteOn ? 0x7F : 0x00) };
        REQUIRE (c.addMessage (msg.data(), 3, 1.0 + 0.0005 * i));
        pushed.push_back (msg);
    }

    dusk::MidiBuffer out;
    out.reserveBytes (dusk::kMidiBlockBytes);
    c.removeNextBlock (out, 512, 2.0);

    std::vector<std::array<std::uint8_t, 3>> got;
    for (const auto meta : out)
    {
        REQUIRE (meta.numBytes == 3);
        got.push_back ({ meta.data[0], meta.data[1], meta.data[2] });
    }

    REQUIRE (got.size() == pushed.size());
    REQUIRE (got == pushed);            // every message, in order
    REQUIRE (got.back()[0] == 0x80);    // including the release at the tail
}

namespace
{
struct Placed
{
    int tag = 0, offset = 0;
    bool operator== (const Placed& o) const { return tag == o.tag && offset == o.offset; }
};

std::vector<Placed> placed (const dusk::MidiBuffer& b)
{
    std::vector<Placed> v;
    for (const auto meta : b)
        v.push_back ({ meta.data[0], meta.samplePosition });
    return v;
}

constexpr int kSubBlock = 512;
constexpr int kSplitCycle = 4 * kSubBlock;
constexpr double kCycleMs = 1000.0 * kSplitCycle / 48000.0;

// One cycle handed over as four blocks, all run when the cycle starts. The
// clock a later block reads is a little on, as the real one is.
std::array<std::vector<Placed>, 4> drainSplitCycle (MidiCollector& c, double cycleStartMs)
{
    std::array<std::vector<Placed>, 4> blocks;
    for (int b = 0; b < 4; ++b)
    {
        dusk::MidiBuffer out;
        c.removeNextSubBlock (out, kSubBlock, b * kSubBlock, kSplitCycle, cycleStartMs + 0.02 * b);
        blocks[(std::size_t) b] = placed (out);
    }
    return blocks;
}
} // namespace

TEST_CASE ("MidiCollector places a split cycle's input in the block its arrival maps to",
           "[midi][collector]")
{
    MidiCollector split, whole;
    split.reset (48000.0, 0.0);
    whole.reset (48000.0, 0.0);
    // Arrivals spread over the cycle before this one: samples 48, 576, 1104,
    // 1632 and 2016 of its 2048.
    const std::array<double, 5> arrivals { 1.0, 12.0, 23.0, 34.0, 42.0 };
    for (std::size_t i = 0; i < arrivals.size(); ++i)
    {
        add (split, (std::uint8_t) (i + 1), arrivals[i]);
        add (whole, (std::uint8_t) (i + 1), arrivals[i]);
    }

    const auto blocks = drainSplitCycle (split, kCycleMs);
    CHECK (blocks[0] == std::vector<Placed> { { 1, 48 } });
    CHECK (blocks[1] == std::vector<Placed> { { 2, 64 } });
    CHECK (blocks[2] == std::vector<Placed> { { 3, 80 } });
    CHECK (blocks[3] == std::vector<Placed> { { 4, 96 }, { 5, 480 } });

    // The same input taken as one block of the whole cycle lands on the same
    // samples of it.
    dusk::MidiBuffer cycle;
    whole.removeNextBlock (cycle, kSplitCycle, kCycleMs);
    std::vector<Placed> joined;
    for (int b = 0; b < 4; ++b)
        for (const auto& e : blocks[(std::size_t) b])
            joined.push_back ({ e.tag, e.offset + b * kSubBlock });
    CHECK (joined == placed (cycle));

    // The later blocks did not move the clock: the next cycle is retimed from
    // the start of this one.
    add (split, 6, kCycleMs + 13.0);   // sample 624 of the next cycle
    const auto next = drainSplitCycle (split, 2.0 * kCycleMs);
    CHECK (next[0].empty());
    CHECK (next[1] == std::vector<Placed> { { 6, 112 } });
    CHECK (next[2].empty());
    CHECK (next[3].empty());
}

TEST_CASE ("MidiCollector takes a block that is a whole cycle by the clock", "[midi][collector]")
{
    MidiCollector viaSubBlock, direct;
    viaSubBlock.reset (48000.0, 0.0);
    direct.reset (48000.0, 0.0);
    for (auto* c : { &viaSubBlock, &direct })
    {
        add (*c, 1, 0.0);
        add (*c, 2, 2.0);
        add (*c, 3, 5.0);
    }
    const int cycleSamples = GENERATE (0, 512);
    dusk::MidiBuffer a, b;
    viaSubBlock.removeNextSubBlock (a, 512, 0, cycleSamples, 10.0);
    direct.removeNextBlock (b, 512, 10.0);
    REQUIRE (placed (a) == placed (b));
    REQUIRE (placed (a).size() == 3);
}

// The engine can sit out the first blocks of a cycle (it is suspended while a
// device is reconfigured). What the collector holds is then the cycle before,
// already played.
TEST_CASE ("MidiCollector does not replay a cycle whose successor's start was skipped",
           "[midi][collector]")
{
    MidiCollector c;
    c.reset (48000.0, 0.0);
    add (c, 1, 12.0);   // sample 576: block 1
    add (c, 2, 23.0);   // sample 1104: block 2
    const auto first = drainSplitCycle (c, kCycleMs);
    REQUIRE (first[1] == std::vector<Placed> { { 1, 64 } });
    REQUIRE (first[2] == std::vector<Placed> { { 2, 80 } });

    add (c, 3, kCycleMs + 12.0);
    for (int b = 1; b < 4; ++b)
    {
        dusk::MidiBuffer out;
        c.removeNextSubBlock (out, kSubBlock, b * kSubBlock, kSplitCycle, 2.0 * kCycleMs);
        CHECK (out.isEmpty());
    }

    // What arrived meanwhile is still pending, and plays once.
    const auto third = drainSplitCycle (c, 3.0 * kCycleMs);
    std::vector<int> tags;
    for (const auto& block : third)
        for (const auto& e : block)
            tags.push_back (e.tag);
    CHECK (tags == std::vector<int> { 3 });
}


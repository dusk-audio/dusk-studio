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

// Block `b` of four of the cycle numbered `serial`.
std::vector<Placed> drainBlockOf (MidiCollector& c, std::uint32_t serial, int b, double nowMs)
{
    dusk::MidiBuffer out;
    c.removeNextSubBlock (out, kSubBlock, { b * kSubBlock, kSplitCycle, serial }, nowMs);
    return placed (out);
}

// One cycle handed over as four blocks, all run when the cycle starts. The
// clock a later block reads is a little on, as the real one is.
std::array<std::vector<Placed>, 4> drainSplitCycle (MidiCollector& c, std::uint32_t serial, double cycleStartMs)
{
    std::array<std::vector<Placed>, 4> blocks;
    for (int b = 0; b < 4; ++b)
        blocks[(std::size_t) b] = drainBlockOf (c, serial, b, cycleStartMs + 0.02 * b);
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

    const auto blocks = drainSplitCycle (split, 1, kCycleMs);
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
    const auto next = drainSplitCycle (split, 2, 2.0 * kCycleMs);
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
    viaSubBlock.removeNextSubBlock (a, 512, { 0, cycleSamples, 0 }, 10.0);
    direct.removeNextBlock (b, 512, 10.0);
    REQUIRE (placed (a) == placed (b));
    REQUIRE (placed (a).size() == 3);
}

namespace
{
struct Note
{
    int status = 0, key = 0, offset = 0;
    bool operator== (const Note& o) const { return status == o.status && key == o.key && offset == o.offset; }
};

void addNote (MidiCollector& c, bool on, int key, double timeMs)
{
    const std::uint8_t bytes[3] { (std::uint8_t) (on ? 0x90 : 0x80), (std::uint8_t) key, (std::uint8_t) (on ? 100 : 0) };
    c.addMessage (bytes, 3, timeMs);
}

std::vector<Note> notesOf (MidiCollector& c, std::uint32_t serial, int block, double nowMs)
{
    dusk::MidiBuffer out;
    c.removeNextSubBlock (out, kSubBlock, { block * kSubBlock, kSplitCycle, serial }, nowMs);
    std::vector<Note> v;
    for (const auto meta : out)
        v.push_back ({ meta.data[0], meta.data[1], meta.samplePosition });
    return v;
}

constexpr int kOn = 0x90, kOff = 0x80;
} // namespace

// The engine can sit blocks out (it is suspended while a device is
// reconfigured), which leaves the collector holding events for blocks that
// never ran. They are not played where they were placed, in whatever block
// comes next at that offset: a note-on would sound seconds late. Nor are they
// all dropped: a note-off among them ends a note an earlier block started.
TEST_CASE ("MidiCollector ends, and does not start, notes left over from blocks that never ran",
           "[midi][collector]")
{
    MidiCollector c;
    c.reset (48000.0, 0.0);
    addNote (c, true, 60, 12.0);    // sample 576: block 1
    addNote (c, false, 60, 23.0);   // sample 1104: block 2
    addNote (c, true, 64, 34.0);    // sample 1632: block 3
    REQUIRE (notesOf (c, 1, 0, kCycleMs).empty());
    REQUIRE (notesOf (c, 1, 1, kCycleMs) == std::vector<Note> { { kOn, 60, 64 } });
    // Blocks 2 and 3 of cycle 1 never run. This arrives meanwhile.
    addNote (c, true, 67, kCycleMs + 12.0);

    SECTION ("back at the same block of a later cycle")
    {
        // By its offset this block follows the last one played: only the
        // cycle's number says it is another cycle.
        CHECK (notesOf (c, 7, 2, 3.0 * kCycleMs) == std::vector<Note> { { kOff, 60, 0 } });
        CHECK (notesOf (c, 7, 3, 3.0 * kCycleMs).empty());
    }
    SECTION ("back at the start of the next cycle")
    {
        // The held note-off first, then the new cycle's own events.
        const auto block0 = notesOf (c, 2, 0, 2.0 * kCycleMs);
        REQUIRE (block0.size() == 1);
        CHECK (block0[0] == Note { kOff, 60, 0 });
        CHECK (notesOf (c, 2, 1, 2.0 * kCycleMs) == std::vector<Note> { { kOn, 67, 64 } });
        CHECK (notesOf (c, 2, 2, 2.0 * kCycleMs).empty());
        CHECK (notesOf (c, 2, 3, 2.0 * kCycleMs).empty());
        return;
    }
    SECTION ("back in the same cycle, a block on")
    {
        CHECK (notesOf (c, 1, 3, kCycleMs) == std::vector<Note> { { kOff, 60, 0 }, { kOn, 64, 96 } });
    }
    SECTION ("back once cycles are whole again")
    {
        dusk::MidiBuffer out;
        c.removeNextSubBlock (out, kSplitCycle, {}, 2.0 * kCycleMs);
        std::vector<Note> got;
        for (const auto meta : out)
            got.push_back ({ meta.data[0], meta.data[1], meta.samplePosition });
        CHECK (got == std::vector<Note> { { kOff, 60, 0 }, { kOn, 67, 576 } });
        return;
    }

    // What arrived meanwhile was never held: it plays once, in a cycle that
    // is drained from its start.
    std::vector<Note> later;
    for (int b = 0; b < 4; ++b)
        for (const auto& n : notesOf (c, 9, b, 4.0 * kCycleMs))
            later.push_back (n);
    REQUIRE (later.size() == 1);
    CHECK (later[0].status == kOn);
    CHECK (later[0].key == 67);
}

// A cycle played to its end leaves nothing behind for a later one.
TEST_CASE ("MidiCollector keeps nothing of a cycle it played in full", "[midi][collector]")
{
    MidiCollector c;
    c.reset (48000.0, 0.0);
    addNote (c, true, 60, 12.0);
    addNote (c, false, 60, 34.0);
    std::size_t played = 0;
    for (int b = 0; b < 4; ++b)
        played += notesOf (c, 1, b, kCycleMs).size();
    REQUIRE (played == 2);
    for (int b = 1; b < 4; ++b)
        CHECK (notesOf (c, 2, b, 2.0 * kCycleMs).empty());
    CHECK (notesOf (c, 3, 0, 3.0 * kCycleMs).empty());
}

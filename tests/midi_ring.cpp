#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "foundation/MidiRing.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using dusk::MidiRing;
using Catch::Matchers::WithinAbs;

namespace
{
// One drained record captured for comparison.
struct Rec { std::vector<std::uint8_t> bytes; double timeMs; };

std::vector<Rec> drainAll (MidiRing& r)
{
    std::vector<Rec> out;
    r.drain ([&] (const std::uint8_t* b, int n, double t)
    {
        out.push_back ({ std::vector<std::uint8_t> (b, b + n), t });
    });
    return out;
}

std::vector<std::uint8_t> pattern (int seed, int n)
{
    std::vector<std::uint8_t> v ((std::size_t) n);
    for (int i = 0; i < n; ++i)
        v[(std::size_t) i] = (std::uint8_t) ((seed * 31 + i * 7) & 0xFF);
    return v;
}
} // namespace

TEST_CASE ("MidiRing round-trips records byte-exact, order preserved", "[midi][ring]")
{
    MidiRing r (4096);

    const std::vector<std::vector<std::uint8_t>> msgs = {
        { 0x90, 0x40, 0x7F },
        { 0xF8 },
        { 0xB0, 0x07, 0x64 },
    };
    // A sysex-sized record too - larger than any channel message.
    const auto sysex = pattern (5, 300);

    for (int i = 0; i < (int) msgs.size(); ++i)
        REQUIRE (r.push (msgs[(std::size_t) i].data(), (int) msgs[(std::size_t) i].size(),
                         100.0 + i));
    REQUIRE (r.push (sysex.data(), (int) sysex.size(), 999.0));

    auto got = drainAll (r);
    REQUIRE (got.size() == msgs.size() + 1);
    for (int i = 0; i < (int) msgs.size(); ++i)
    {
        REQUIRE (got[(std::size_t) i].bytes == msgs[(std::size_t) i]);
        REQUIRE_THAT (got[(std::size_t) i].timeMs, WithinAbs (100.0 + i, 1e-9));
    }
    REQUIRE (got.back().bytes == sysex);
    REQUIRE_THAT (got.back().timeMs, WithinAbs (999.0, 1e-9));
    REQUIRE (r.isEmpty());
}

TEST_CASE ("MidiRing stitches records that wrap the physical end", "[midi][ring]")
{
    // Small capacity so records repeatedly straddle the buffer boundary; varying
    // payload sizes move the split point around.
    MidiRing r (64);

    for (int i = 0; i < 200; ++i)
    {
        const int n = 1 + (i % 7);          // 1..7 payload bytes
        const auto p = pattern (i, n);
        REQUIRE (r.push (p.data(), n, (double) i));

        auto got = drainAll (r);
        REQUIRE (got.size() == 1u);
        REQUIRE (got[0].bytes == p);
        REQUIRE_THAT (got[0].timeMs, WithinAbs ((double) i, 1e-9));
    }
}

TEST_CASE ("MidiRing fill / drain / refill preserves everything", "[midi][ring]")
{
    MidiRing r (256);

    std::vector<std::vector<std::uint8_t>> pushed;
    // Fill until full.
    for (int i = 0; ; ++i)
    {
        const auto p = pattern (i, 3 + (i % 4));
        if (! r.push (p.data(), (int) p.size(), (double) i)) break;
        pushed.push_back (p);
    }
    REQUIRE (pushed.size() > 3u);

    auto got = drainAll (r);
    REQUIRE (got.size() == pushed.size());
    for (std::size_t i = 0; i < pushed.size(); ++i)
        REQUIRE (got[i].bytes == pushed[i]);

    // Refill after a full drain - head/tail are now deep into the counter space.
    pushed.clear();
    for (int i = 0; ; ++i)
    {
        const auto p = pattern (i + 500, 2 + (i % 5));
        if (! r.push (p.data(), (int) p.size(), (double) i)) break;
        pushed.push_back (p);
    }
    got = drainAll (r);
    REQUIRE (got.size() == pushed.size());
    for (std::size_t i = 0; i < pushed.size(); ++i)
        REQUIRE (got[i].bytes == pushed[i]);
}

TEST_CASE ("MidiRing overflow drops the whole record and stays intact", "[midi][ring]")
{
    MidiRing r (64);

    // A record larger than the whole ring is always refused.
    const auto huge = pattern (1, 200);
    REQUIRE_FALSE (r.push (huge.data(), (int) huge.size(), 1.0));

    // Fill with 3-byte records (13 bytes each) until one is dropped.
    std::vector<std::vector<std::uint8_t>> accepted;
    for (int i = 0; i < 100; ++i)
    {
        const auto p = pattern (i, 3);
        if (r.push (p.data(), 3, (double) i))
            accepted.push_back (p);
        else
            break;
    }
    REQUIRE (accepted.size() >= 1u);

    // The accepted records survive the overflow unharmed and in order.
    auto got = drainAll (r);
    REQUIRE (got.size() == accepted.size());
    for (std::size_t i = 0; i < accepted.size(); ++i)
        REQUIRE (got[i].bytes == accepted[i]);
}

TEST_CASE ("MidiRing clear empties pending records", "[midi][ring]")
{
    MidiRing r (128);
    const std::uint8_t note[] = { 0x90, 0x3C, 0x40 };
    REQUIRE (r.push (note, 3, 1.0));
    REQUIRE (r.push (note, 3, 2.0));
    REQUIRE_FALSE (r.isEmpty());

    r.clear();
    REQUIRE (r.isEmpty());
    REQUIRE (drainAll (r).empty());

    // Usable again after clear.
    REQUIRE (r.push (note, 3, 3.0));
    auto got = drainAll (r);
    REQUIRE (got.size() == 1u);
    REQUIRE_THAT (got[0].timeMs, WithinAbs (3.0, 1e-9));
}

TEST_CASE ("MidiRing cursor bounds a scan+drain, leaving later pushes pending", "[midi][ring]")
{
    MidiRing r (256);
    const std::uint8_t a[] = { 0x90, 0x40, 0x7F };
    const std::uint8_t b[] = { 0x80, 0x40, 0x00 };

    REQUIRE (r.push (a, 3, 1.0));
    const std::size_t cursor = r.producerCursor();
    REQUIRE (r.push (b, 3, 2.0));            // pushed "after the scan"

    // A scan bounded to the cursor sees only the first record. forEachUntil is
    // noexcept, so capture the timestamp and assert after it returns rather than
    // letting a failing REQUIRE throw out of the callback.
    int    scanned = 0;
    double scannedTime = 0.0;
    r.forEachUntil (cursor, [&] (int, double t)
    {
        ++scanned;
        scannedTime = t;
    });
    REQUIRE (scanned == 1);
    REQUIRE_THAT (scannedTime, WithinAbs (1.0, 1e-9));

    // A drain bounded to the same cursor consumes only the first record.
    std::vector<Rec> drained;
    r.drainUntil (cursor, [&] (const std::uint8_t* p, int n, double t)
    {
        drained.push_back ({ std::vector<std::uint8_t> (p, p + n), t });
    });
    REQUIRE (drained.size() == 1u);
    REQUIRE (drained[0].bytes == std::vector<std::uint8_t> (a, a + 3));

    // The message pushed after the cursor survives for the next pass.
    REQUIRE_FALSE (r.isEmpty());
    auto rest = drainAll (r);
    REQUIRE (rest.size() == 1u);
    REQUIRE (rest[0].bytes == std::vector<std::uint8_t> (b, b + 3));
    REQUIRE_THAT (rest[0].timeMs, WithinAbs (2.0, 1e-9));
}

TEST_CASE ("MidiRing deterministic interleave preserves FIFO / integrity across the wrap", "[midi][ring]")
{
    // Single-threaded model of the SPSC contract: interleave pushes with
    // cursor-bounded drains so head and tail chase each other across the wrap
    // boundary many times. Bounded loops, no threads (the raw memory-ordering
    // guarantee under real contention belongs to TSan / hardware, not the unit
    // suite).
    MidiRing r (128);

    int seq = 0;
    std::vector<Rec> pending;   // pushed, not yet drained (FIFO)

    auto pushOne = [&]
    {
        const int n = 1 + (seq % 6);
        const auto p = pattern (seq, n);
        if (! r.push (p.data(), n, (double) seq)) return false;
        pending.push_back ({ p, (double) seq });
        ++seq;
        return true;
    };

    for (int round = 0; round < 400; ++round)
    {
        // Push a burst, snapshot the cursor (everything pending is drainable to
        // here), then push more past the cursor.
        for (int k = 0, b = 1 + (round % 5); k < b; ++k) pushOne();
        const std::size_t cut = r.producerCursor();
        const std::size_t drainable = pending.size();
        for (int k = 0, b = 1 + ((round + 1) % 4); k < b; ++k) pushOne();

        // Drain exactly the pre-cut records; assert order + integrity after the
        // callback returns (drainUntil is noexcept).
        std::vector<Rec> got;
        r.drainUntil (cut, [&] (const std::uint8_t* bytes, int n, double t)
        {
            got.push_back ({ std::vector<std::uint8_t> (bytes, bytes + n), t });
        });
        REQUIRE (got.size() == drainable);
        for (std::size_t i = 0; i < got.size(); ++i)
        {
            REQUIRE (got[i].bytes == pending[i].bytes);
            REQUIRE_THAT (got[i].timeMs, WithinAbs (pending[i].timeMs, 1e-9));
        }
        pending.erase (pending.begin(), pending.begin() + (std::ptrdiff_t) got.size());
    }

    // Whatever is still queued drains in order.
    auto rest = drainAll (r);
    REQUIRE (rest.size() == pending.size());
    for (std::size_t i = 0; i < rest.size(); ++i)
        REQUIRE (rest[i].bytes == pending[i].bytes);
    REQUIRE (seq > 400);   // a real volume of records moved through the ring
}

TEST_CASE ("MidiRing full-ring retry: refused push succeeds after a drain frees space", "[midi][ring]")
{
    MidiRing r (128);
    const std::uint8_t note[] = { 0x90, 0x40, 0x7F };

    int accepted = 0;
    while (r.push (note, 3, (double) accepted)) ++accepted;
    REQUIRE (accepted > 0);

    // Now full: the next push is refused.
    REQUIRE_FALSE (r.push (note, 3, 999.0));

    // Draining frees space; the retried push then succeeds.
    (void) drainAll (r);
    REQUIRE (r.push (note, 3, 999.0));
}

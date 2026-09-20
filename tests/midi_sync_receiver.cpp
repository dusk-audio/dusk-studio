#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/MidiSyncReceiver.h"
#include "foundation/MidiBuffer.h"

#include <cstdint>

using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 48000.0;

// Samples between F8 clocks at a tempo: 24 clocks to the quarter note.
double clockInterval (double bpm) { return kRate * 60.0 / (bpm * 24.0); }

// Hands the receiver one clock tick at an absolute sample position.
void tickAt (duskstudio::MidiSyncReceiver& receiver, std::int64_t sample)
{
    const std::uint8_t clock = 0xF8;
    dusk::MidiBuffer buffer;
    buffer.addEvent (&clock, 1, 0);
    receiver.process (buffer, sample);
}

// A steady stream at `bpm`, starting at `from`, returning where it ended.
std::int64_t runClock (duskstudio::MidiSyncReceiver& receiver, double bpm, int ticks,
                       std::int64_t from = 0)
{
    const auto interval = clockInterval (bpm);
    for (int i = 0; i < ticks; ++i)
        tickAt (receiver, from + (std::int64_t) (interval * (double) i));
    return from + (std::int64_t) (interval * (double) (ticks - 1));
}

void sendByte (duskstudio::MidiSyncReceiver& receiver, std::uint8_t byte)
{
    dusk::MidiBuffer buffer;
    buffer.addEvent (&byte, 1, 0);
    receiver.process (buffer, 0);
}
} // namespace

// The manual promises the tempo is chased from the F8 clock. There is nothing
// to report until two clocks have marked out an interval.
TEST_CASE ("MidiSyncReceiver chases the tempo of the incoming clock", "[midiclock][sync]")
{
    using duskstudio::MidiSyncReceiver;

    MidiSyncReceiver receiver;
    receiver.prepare (kRate);
    CHECK_THAT (receiver.getBpm(), WithinAbs (0.0f, 1.0e-6f));

    tickAt (receiver, 0);
    CHECK_THAT (receiver.getBpm(), WithinAbs (0.0f, 1.0e-6f));

    runClock (receiver, 120.0, MidiSyncReceiver::kAvgWindow + 1);
    CHECK_THAT (receiver.getBpm(), WithinAbs (120.0f, 0.5f));

    MidiSyncReceiver slower;
    slower.prepare (kRate);
    runClock (slower, 90.0, MidiSyncReceiver::kAvgWindow + 1);
    CHECK_THAT (slower.getBpm(), WithinAbs (90.0f, 0.5f));
}

// "Averaged over 24 ticks" is what keeps a jittery cable from wobbling the
// tempo readout: intervals a fifth long and a fifth short are inside the
// rejection threshold, so the average is the only thing steadying them.
TEST_CASE ("MidiSyncReceiver averages jitter out of the reported tempo", "[midiclock][sync]")
{
    using duskstudio::MidiSyncReceiver;

    MidiSyncReceiver receiver;
    receiver.prepare (kRate);

    const auto nominal = clockInterval (120.0);
    double at = 0.0;
    for (int i = 0; i < MidiSyncReceiver::kAvgWindow * 3; ++i)
    {
        tickAt (receiver, (std::int64_t) at);
        at += nominal * (i % 2 == 0 ? 1.2 : 0.8);
    }

    // Either interval on its own reads 100 or 150 BPM; the window holds an
    // equal number of each, so the tempo sits on the nominal.
    CHECK_THAT (receiver.getBpm(), WithinAbs (120.0f, 1.0f));
}

// A single wild interval is a dropout, not a tempo change, and the manual says
// those are skipped.
TEST_CASE ("MidiSyncReceiver skips a clock interval that jumps", "[midiclock][sync]")
{
    using duskstudio::MidiSyncReceiver;

    MidiSyncReceiver receiver;
    receiver.prepare (kRate);
    const auto last = runClock (receiver, 120.0, MidiSyncReceiver::kAvgWindow + 1);
    const auto steady = receiver.getBpm();
    REQUIRE_THAT (steady, WithinAbs (120.0f, 0.5f));

    // A gap of four normal intervals: far past the rejection factor.
    tickAt (receiver, last + (std::int64_t) (clockInterval (120.0) * 4.0));
    CHECK_THAT (receiver.getBpm(), WithinAbs (steady, 1.0e-6f));

    // The stream recovers on its own once the clock is steady again.
    runClock (receiver, 120.0, MidiSyncReceiver::kAvgWindow + 1,
              last + (std::int64_t) (clockInterval (120.0) * 8.0));
    CHECK_THAT (receiver.getBpm(), WithinAbs (120.0f, 0.5f));
}

// Start, Continue and Stop drive the rolling hint the engine chases.
TEST_CASE ("MidiSyncReceiver follows the transport bytes", "[midiclock][sync]")
{
    using duskstudio::MidiSyncReceiver;

    MidiSyncReceiver receiver;
    receiver.prepare (kRate);
    CHECK_FALSE (receiver.isRolling());

    sendByte (receiver, 0xFA);   // Start
    CHECK (receiver.isRolling());
    sendByte (receiver, 0xFC);   // Stop
    CHECK_FALSE (receiver.isRolling());
    sendByte (receiver, 0xFB);   // Continue
    CHECK (receiver.isRolling());

    // A reset drops the history and the hint with it.
    receiver.reset();
    CHECK_FALSE (receiver.isRolling());
    CHECK_THAT (receiver.getBpm(), WithinAbs (0.0f, 1.0e-6f));
}

#include <catch2/catch_test_macros.hpp>

#include "foundation/MidiBuffer.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdint>
#include <vector>

// dusk::MidiBuffer must iterate events in the same order and expose the same
// raw bytes + sample positions as juce::MidiBuffer, so the decoders read the
// identical byte stream after the engine's juce->dusk bridge.
TEST_CASE ("dusk::MidiBuffer iterates like juce::MidiBuffer", "[foundation][midi]")
{
    struct Ev { std::vector<std::uint8_t> bytes; int pos; };
    const std::vector<Ev> events = {
        { { 0xF1, 0x03 },                               10 },   // MTC quarter-frame
        { { 0xF8 },                                     16 },   // clock (1 byte)
        { { 0x90, 0x40, 0x7F },                         32 },   // note-on
        { { 0xF0,0x7F,0x7F,0x01,0x01,0x21,0x00,0x00,0x00,0xF7 }, 5 }, // MTC full-frame sysex
        { { 0xFC },                                     40 },   // stop
    };

    juce::MidiBuffer j;
    for (const auto& e : events)
        j.addEvent (e.bytes.data(), (int) e.bytes.size(), e.pos);

    // Mirror the engine bridge: fill the dusk buffer by walking the (already
    // sorted) juce buffer, so ordering comes from the source exactly as in
    // AudioEngine.
    dusk::MidiBuffer d;
    for (const auto meta : j)
        d.addEvent (meta.getMessage().getRawData(), meta.getMessage().getRawDataSize(),
                    meta.samplePosition);

    auto ji = j.begin();
    auto di = d.begin();
    int count = 0;
    for (; di != d.end(); ++di, ++ji)
    {
        REQUIRE (ji != j.end());
        const auto jm = (*ji).getMessage();
        const auto dm = (*di).getMessage();

        REQUIRE ((*di).samplePosition == (*ji).samplePosition);
        REQUIRE (dm.getRawDataSize()  == jm.getRawDataSize());
        for (int b = 0; b < dm.getRawDataSize(); ++b)
            REQUIRE (dm.getRawData()[b] == jm.getRawData()[b]);

        // The native plugin hosts iterate via meta.data / meta.numBytes (the
        // juce::MidiMessageMetadata shape); these must match the message view.
        REQUIRE ((*di).numBytes == dm.getRawDataSize());
        REQUIRE ((*di).data     == dm.getRawData());
        REQUIRE ((*di).data     != nullptr);
        for (int b = 0; b < (*di).numBytes; ++b)
            REQUIRE ((*di).data[b] == jm.getRawData()[b]);
        ++count;
    }
    REQUIRE (ji == j.end());
    REQUIRE (count == (int) events.size());
}

TEST_CASE ("dusk::MidiBuffer clear keeps capacity and empties", "[foundation][midi]")
{
    dusk::MidiBuffer d;
    d.reserveBytes (256);
    const std::uint8_t clk = 0xF8;
    d.addEvent (&clk, 1, 0);
    REQUIRE (! d.isEmpty());

    d.clear();
    REQUIRE (d.isEmpty());
    int n = 0;
    for (auto it = d.begin(); it != d.end(); ++it) ++n;
    REQUIRE (n == 0);

    // The reserved cap has to survive the clear, or the RT path would start
    // reallocating on the audio thread after the first block. It is only
    // observable through the drop it causes, and the fill is bounded so a lost
    // cap fails here rather than growing until the runner is killed.
    constexpr int kMoreThanFits = 256;
    int accepted = 0;
    while (accepted < kMoreThanFits && d.addEvent (&clk, 1, accepted)) ++accepted;
    REQUIRE (accepted > 0);
    REQUIRE (accepted < kMoreThanFits);
    REQUIRE (! d.addEvent (&clk, 1, 0));
}

TEST_CASE ("dusk::MidiBuffer drops past its reserved cap instead of growing",
           "[foundation][midi]")
{
    // The RT out-queue relies on this: a reserved buffer must refuse an event
    // that would exceed the cap rather than reallocate on the audio thread.
    constexpr std::size_t kCap = 64;
    dusk::MidiBuffer d;
    d.reserveBytes (kCap);

    const std::uint8_t note[3] { 0x90, 60, 100 };
    // 8-byte header + 3 payload bytes per event. Bounded well past that so a
    // buffer that wrongly grows fails the count instead of running away.
    constexpr int kPerEvent = 11;
    constexpr int kMoreThanFits = (int) (kCap / kPerEvent) + 8;
    int accepted = 0;
    while (accepted < kMoreThanFits && d.addEvent (note, 3, accepted)) ++accepted;

    REQUIRE (accepted == (int) (kCap / kPerEvent));

    // Rejection leaves the existing contents intact and is repeatable.
    int counted = 0;
    for (const auto meta : d)
    {
        REQUIRE (meta.numBytes == 3);
        REQUIRE (meta.samplePosition == counted);
        ++counted;
    }
    REQUIRE (counted == accepted);
    REQUIRE (! d.addEvent (note, 3, 0));

    // Unreserved buffers keep growing freely (message-thread use).
    dusk::MidiBuffer unbounded;
    for (int i = 0; i < 1000; ++i)
        REQUIRE (unbounded.addEvent (note, 3, i));
}

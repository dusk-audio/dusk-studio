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
}

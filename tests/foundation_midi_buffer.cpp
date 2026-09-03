#include <catch2/catch_test_macros.hpp>

#include "foundation/MidiBuffer.h"
#include "engine/midi/MidiSort.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
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

// copyEventsWhole is the RT bridge every capped scratch buffer refills through
// (the native-host bridge in ChannelStrip, the OOP scratch in PluginSlot). Those
// call sites need the full engine to exercise, so the policy is asserted here on
// the shared helper instead.
TEST_CASE ("dusk::copyEventsWhole delivers a block whole or not at all",
           "[foundation][midi]")
{
    const std::uint8_t on[3]  { 0x90, 60, 100 };
    const std::uint8_t off[3] { 0x80, 60, 0 };

    dusk::MidiBuffer src;
    src.addEvent (on,  3, 0);
    src.addEvent (off, 3, 64);

    SECTION ("a block that fits copies through unchanged")
    {
        dusk::MidiBuffer dest;
        dest.reserveBytes (64);
        dusk::copyEventsWhole (src, dest);

        int n = 0;
        for (const auto meta : dest)
        {
            REQUIRE (meta.numBytes == 3);
            REQUIRE (meta.data[0] == (n == 0 ? 0x90 : 0x80));
            ++n;
        }
        REQUIRE (n == 2);
    }

    SECTION ("a block that overruns the cap arrives empty, not truncated")
    {
        // Room for the note-on alone: delivering it without the note-off would
        // hang the note.
        dusk::MidiBuffer dest;
        dest.reserveBytes (11);
        dusk::copyEventsWhole (src, dest);
        REQUIRE (dest.isEmpty());
    }

    SECTION ("a record too big for the cap drops alone")
    {
        // A bulk sysex no cap could ever hold must not silence the notes around
        // it - it is undeliverable however the block is cut.
        std::vector<std::uint8_t> sysex (200, 0x7F);
        sysex.front() = 0xF0;
        sysex.back()  = 0xF7;

        dusk::MidiBuffer big;
        big.addEvent (on, 3, 0);
        big.addEvent (sysex.data(), (int) sysex.size(), 32);
        big.addEvent (off, 3, 64);

        dusk::MidiBuffer dest;
        dest.reserveBytes (64);
        REQUIRE_FALSE (dest.fitsWhenEmpty ((int) sysex.size()));
        dusk::copyEventsWhole (big, dest);

        int n = 0;
        for (const auto meta : dest)
        {
            REQUIRE (meta.numBytes == 3);
            ++n;
        }
        REQUIRE (n == 2);
    }
}

TEST_CASE ("AudioEngine MIDI output scratch matches the per-track capacity",
           "[foundation][midi][regression][issue-465]")
{
    const auto readSource = [] (const char* relativePath)
    {
        const auto path = std::string (DUSKSTUDIO_SOURCE_DIR) + "/" + relativePath;
        std::ifstream input (path);
        return std::string { std::istreambuf_iterator<char> (input),
                             std::istreambuf_iterator<char>() };
    };

    const auto engineSource = readSource ("src/engine/AudioEngine.cpp");
    const auto devicesSource = readSource ("src/engine/midi/MidiDevices.cpp");

    REQUIRE_FALSE (engineSource.empty());
    REQUIRE_FALSE (devicesSource.empty());
    REQUIRE (engineSource.find (
        "midiOutTrackScratch.reserveBytes (dusk::kMidiRoutingBlockBytes)")
        != std::string::npos);
    REQUIRE (devicesSource.find (
        "slot.events.reserveBytes (dusk::kMidiRoutingBlockBytes)")
        != std::string::npos);
}

TEST_CASE ("MIDI output sorted copy keeps a dense block whole and ordered",
           "[foundation][midi][regression][issue-465]")
{
    SECTION ("a block larger than the old output cap is delivered whole")
    {
        dusk::MidiBuffer source;
        dusk::MidiBuffer destination;
        source.reserveBytes (dusk::kMidiRoutingBlockBytes);
        destination.reserveBytes (dusk::kMidiRoutingBlockBytes);

        std::uint8_t note[3] { 0x90, 0, 100 };
        int inserted = 0;
        while (true)
        {
            note[1] = (std::uint8_t) (inserted % 128);
            if (! source.addEvent (note, 3, 100000 - inserted))
                break;
            ++inserted;
        }
        REQUIRE (inserted > (int) (dusk::kMidiBlockBytes / 11));

        duskstudio::midi::MidiSortScratch scratch;
        duskstudio::midi::copyMidiSorted (source, destination, scratch);

        int count = 0;
        int previousPosition = -1;
        for (const auto meta : destination)
        {
            REQUIRE (meta.samplePosition >= previousPosition);
            previousPosition = meta.samplePosition;
            ++count;
        }
        REQUIRE (count == inserted);
    }

    SECTION ("equal-position events keep their source order")
    {
        dusk::MidiBuffer source;
        dusk::MidiBuffer destination;
        source.reserveBytes (64);
        destination.reserveBytes (64);
        const std::uint8_t first[3]  { 0x90, 60, 100 };
        const std::uint8_t earlier[3] { 0x90, 61, 100 };
        const std::uint8_t second[3] { 0x90, 62, 100 };
        REQUIRE (source.addEvent (first, 3, 5));
        REQUIRE (source.addEvent (earlier, 3, 2));
        REQUIRE (source.addEvent (second, 3, 5));

        duskstudio::midi::MidiSortScratch scratch;
        duskstudio::midi::copyMidiSorted (source, destination, scratch);

        const std::uint8_t expectedNotes[] { 61, 60, 62 };
        int i = 0;
        for (const auto meta : destination)
        {
            REQUIRE (i < 3);
            REQUIRE (meta.data[1] == expectedNotes[i]);
            ++i;
        }
        REQUIRE (i == 3);
    }
}

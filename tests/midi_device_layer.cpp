#include <catch2/catch_test_macros.hpp>

#include <juce_audio_devices/juce_audio_devices.h>
#include "engine/midi/MidiDevices.h"
#include "foundation/MidiBuffer.h"
#include <array>
#include <cstdint>
#include <vector>

using duskstudio::midi::MidiInputClient;
using duskstudio::midi::MidiOutputBank;

TEST_CASE ("MidiInputClient appends a stable Virtual-Keyboard slot", "[midi][devices]")
{
    juce::AudioDeviceManager dm;
    MidiInputClient client;
    client.setDeviceManager (dm);
    client.rebuild (48000.0);

    const int vkb = client.getVirtualKeyboardIndex();
    REQUIRE (vkb >= 0);
    REQUIRE (client.getVirtualKeyboardCollector() != nullptr);

    // VKB is always the last slot, carrying the fixed identifier.
    const auto& devs = client.getDevices();
    REQUIRE (vkb == (int) devs.size() - 1);
    REQUIRE (client.getNumInputs() == (int) devs.size());
    REQUIRE (devs[(size_t) vkb].identifier
             == juce::String (MidiInputClient::kVirtualKeyboardIdentifier));
}

TEST_CASE ("MidiInputClient VKB feed round-trips through the dusk boundary", "[midi][devices]")
{
    juce::AudioDeviceManager dm;
    MidiInputClient client;
    client.setDeviceManager (dm);
    client.rebuild (48000.0);

    const int vkb = client.getVirtualKeyboardIndex();
    auto* col = client.getVirtualKeyboardCollector();
    REQUIRE (col != nullptr);

    // Feed a note-on the way the on-screen keyboard does: an ms-since-epoch
    // timestamp (the collector asserts on a zero stamp and derives the sample
    // offset from it).
    auto noteOn = juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100);
    noteOn.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    col->addMessageToQueue (noteOn);

    constexpr int numSamples = 512;
    dusk::MidiBuffer out;
    out.reserveBytes (4096);
    client.drainBlock (vkb, out, numSamples);

    int count = 0;
    for (const auto meta : out)
    {
        ++count;
        const auto& m = meta.getMessage();
        REQUIRE (m.getRawDataSize() == 3);
        const auto* b = m.getRawData();
        REQUIRE ((b[0] & 0xf0) == 0x90);   // note-on
        REQUIRE ((b[0] & 0x0f) == 0x00);   // channel 1
        REQUIRE (b[1] == 60);
        REQUIRE (b[2] == 100);
        // The collector clamps the offset into the requested block.
        REQUIRE (meta.samplePosition >= 0);
        REQUIRE (meta.samplePosition < numSamples);
    }
    REQUIRE (count == 1);
}

TEST_CASE ("MidiInputClient drainBlock clears and bounds-checks", "[midi][devices]")
{
    juce::AudioDeviceManager dm;
    MidiInputClient client;
    client.setDeviceManager (dm);
    client.rebuild (48000.0);

    dusk::MidiBuffer out;
    out.reserveBytes (256);

    // Out-of-range index just clears the destination.
    const std::uint8_t junk[3] = { 0x90, 1, 1 };
    out.addEvent (junk, 3, 0);
    client.drainBlock (9999, out, 256);
    REQUIRE (out.isEmpty());

    // Empty collector -> empty out.
    client.drainBlock (client.getVirtualKeyboardIndex(), out, 256);
    REQUIRE (out.isEmpty());
}

TEST_CASE ("MidiOutputBank dusk->juce conversion preserves bytes + offsets", "[midi][devices]")
{
    dusk::MidiBuffer in;
    in.reserveBytes (256);
    const std::uint8_t noteOn[3]  = { 0x90, 60, 100 };
    const std::uint8_t cc[3]      = { 0xB0, 7, 127 };
    const std::uint8_t noteOff[3] = { 0x80, 60, 0 };
    in.addEvent (noteOn,  3, 0);
    in.addEvent (cc,      3, 128);
    in.addEvent (noteOff, 3, 400);

    juce::MidiBuffer juceOut;
    MidiOutputBank::toJuceBuffer (in, juceOut);

    struct Ev { int pos; std::array<std::uint8_t, 3> b; };
    std::vector<Ev> got;
    for (const auto meta : juceOut)
    {
        REQUIRE (meta.numBytes == 3);
        got.push_back ({ meta.samplePosition,
                         { meta.data[0], meta.data[1], meta.data[2] } });
    }

    REQUIRE (got.size() == 3);
    // juce::MidiBuffer keeps events sorted by sample position; the inputs were
    // already ascending, so the order carries over 1:1.
    REQUIRE (got[0].pos == 0);   REQUIRE (got[0].b[0] == 0x90);
    REQUIRE (got[1].pos == 128); REQUIRE (got[1].b[0] == 0xB0);
    REQUIRE (got[2].pos == 400); REQUIRE (got[2].b[0] == 0x80);
}

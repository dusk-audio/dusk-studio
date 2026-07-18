#include <catch2/catch_test_macros.hpp>

#include "engine/midi/MidiDevices.h"
#include "foundation/MidiBuffer.h"
#include <cstdint>
#include <string>

using duskstudio::midi::MidiInputClient;
using duskstudio::midi::MidiOutputBank;

// These run against whatever the platform backend enumerates - no assertion
// depends on a device being present, so a headless machine exercises the same
// paths as one with hardware attached.

TEST_CASE ("MidiInputClient appends a stable Virtual-Keyboard slot", "[midi][devices]")
{
    MidiInputClient client;
    client.rebuild (48000.0);

    const int vkb = client.getVirtualKeyboardIndex();
    REQUIRE (vkb >= 0);

    // VKB is always the last slot, carrying the fixed identifier.
    const auto& devs = client.getDevices();
    REQUIRE (vkb == (int) devs.size() - 1);
    REQUIRE (client.getNumInputs() == (int) devs.size());
    REQUIRE (devs[(size_t) vkb].identifier == std::string (MidiInputClient::kVirtualKeyboardIdentifier));
}

TEST_CASE ("MidiInputClient VKB feed round-trips through the dusk boundary", "[midi][devices]")
{
    MidiInputClient client;
    client.rebuild (48000.0);

    const int vkb = client.getVirtualKeyboardIndex();
    REQUIRE (vkb >= 0);

    // Feed a note-on the way the on-screen keyboard does: raw status + data.
    const std::uint8_t noteOn[3] { 0x90, 60, 100 };
    client.postVirtualKeyboardMidi (noteOn, 3);

    constexpr int numSamples = 512;
    dusk::MidiBuffer out;
    out.reserveBytes (4096);
    client.drainBlock (vkb, out, numSamples);

    int count = 0;
    for (const auto meta : out)
    {
        ++count;
        REQUIRE (meta.numBytes == 3);
        REQUIRE (meta.data[0] == 0x90);
        REQUIRE (meta.data[1] == 60);
        REQUIRE (meta.data[2] == 100);
        // The collector clamps the offset into the requested block.
        REQUIRE (meta.samplePosition >= 0);
        REQUIRE (meta.samplePosition < numSamples);
    }
    REQUIRE (count == 1);

    // The drain is destructive - a second one on the same slot yields nothing.
    client.drainBlock (vkb, out, numSamples);
    REQUIRE (out.isEmpty());
}

TEST_CASE ("MidiInputClient drainBlock clears and bounds-checks", "[midi][devices]")
{
    MidiInputClient client;
    client.rebuild (48000.0);

    dusk::MidiBuffer out;
    out.reserveBytes (256);

    // Out-of-range index just clears the destination.
    const std::uint8_t junk[3] { 0x90, 1, 1 };
    REQUIRE (out.addEvent (junk, 3, 0));
    client.drainBlock (9999, out, 256);
    REQUIRE (out.isEmpty());

    REQUIRE (out.addEvent (junk, 3, 0));
    client.drainBlock (-1, out, 256);
    REQUIRE (out.isEmpty());

    // Empty collector -> empty out.
    client.drainBlock (client.getVirtualKeyboardIndex(), out, 256);
    REQUIRE (out.isEmpty());
}

TEST_CASE ("Seam resolves saved identifiers back to bank indices", "[midi][devices]")
{
    MidiInputClient client;
    client.rebuild (48000.0);

    REQUIRE (client.resolveIndex (MidiInputClient::kVirtualKeyboardIdentifier)
             == client.getVirtualKeyboardIndex());
    REQUIRE (client.resolveIndex ("") == -1);
    REQUIRE (client.resolveIndex ("no-such-backend:No Such Client:No Such Port") == -1);

    const auto& inputs = client.getDevices();
    for (int i = 0; i < (int) inputs.size(); ++i)
        REQUIRE (client.resolveIndex (inputs[(size_t) i].identifier) == i);

    MidiOutputBank bank;
    bank.rebuild();
    REQUIRE (bank.resolveIndex ("") == -1);
    REQUIRE (bank.resolveIndex ("no-such-backend:No Such Client:No Such Port") == -1);
    REQUIRE (bank.getNumOutputs() == (int) bank.getDevices().size());
    for (int i = 0; i < bank.getNumOutputs(); ++i)
    {
        REQUIRE (bank.resolveIndex (bank.getDevices()[(size_t) i].identifier) == i);
        REQUIRE (! bank.isOpen (i));   // rebuild never eager-opens
    }
    REQUIRE (! bank.isOpen (-1));
    REQUIRE (! bank.isOpen (bank.getNumOutputs()));
}

TEST_CASE ("MidiOutputBank queueRt drops a block past the slot cap", "[midi][devices]")
{
    MidiOutputBank bank;
    bank.rebuild();

    // No device needed: queueRt only fills a slot, and the pump (not started
    // here) is what would consume it. A block far past the 4 kB slot cap must be
    // dropped whole rather than reallocated on the audio thread, and the queue
    // must stay usable afterwards.
    dusk::MidiBuffer big;
    big.reserveBytes (1 << 16);
    const std::uint8_t note[3] { 0x90, 64, 100 };
    for (int i = 0; i < 4096; ++i)
        REQUIRE (big.addEvent (note, 3, i));

    for (int i = 0; i < 128; ++i)   // twice the queue depth: nothing may commit
        bank.queueRt (0, big, 48000.0);

    dusk::MidiBuffer small;
    small.reserveBytes (256);
    REQUIRE (small.addEvent (note, 3, 0));
    for (int i = 0; i < 128; ++i)   // past the queue depth: drops, never grows
        bank.queueRt (0, small, 48000.0);
}

#include <catch2/catch_test_macros.hpp>

#include "engine/midi/MidiBackend.h"
#include "engine/midi/MidiDevices.h"
#include "foundation/MidiBuffer.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

// What queueRt actually delivers is asserted end to end in alsa_seq_midi.cpp,
// where a real port receives the bytes; there is no way to observe it here
// without a device.

namespace
{
constexpr std::uint8_t kProbeNote[3] { 0x90, 64, 96 };

// A backend at its most hostile: at the moment the seam subscribes a source it
// inspects the bank that source's bytes would have to route through, and hands
// the seam a first byte-run to route right then.
class ProbeBackend final : public duskstudio::midi::IMidiInputBackend
{
public:
    std::vector<duskstudio::midi::BackendDeviceInfo> enumerate() override
    {
        return { { "Probe A", "probe:a" }, { "Probe B", "probe:b" } };
    }

    void setReceiver (Receiver r) override { receiver = std::move (r); }
    std::string migrateIdentifier (const std::string&) override { return {}; }

    bool enable (const std::string& identifier) override
    {
        ++enableCount;
        enabledWhileDispatching = enabledWhileDispatching || dispatching;

        if (client != nullptr)
        {
            // This source's own route, and the bank as a whole: the synthetic
            // slot is appended last, so it sitting at the end is what says the
            // collector vector is finished rather than mid-append.
            const int idx = client->resolveIndex (identifier);
            routeReady  = routeReady && idx >= 0 && idx < client->getNumInputs();
            bankBuilt   = bankBuilt
                            && client->getVirtualKeyboardIndex() == client->getNumInputs() - 1;
        }

        if (receiver)
            receiver (identifier, kProbeNote, 3, duskstudio::midi::backendClockMs());
        return true;
    }

    void disableAll() override {}
    void start() override { dispatching = true; }
    void stop()  override { dispatching = false; ++stopCount; }

    MidiInputClient* client      = nullptr;
    Receiver receiver;
    bool dispatching             = false;
    bool enabledWhileDispatching = false;
    bool routeReady              = true;
    bool bankBuilt               = true;
    int  enableCount             = 0;
    int  stopCount               = 0;
};

int countProbeNotes (const dusk::MidiBuffer& buf)
{
    int count = 0;
    for (const auto meta : buf)
    {
        ++count;
        REQUIRE (meta.numBytes == 3);
        REQUIRE (meta.data[0] == kProbeNote[0]);
        REQUIRE (meta.data[1] == kProbeNote[1]);
        REQUIRE (meta.data[2] == kProbeNote[2]);
    }
    return count;
}
} // namespace

TEST_CASE ("MidiInputClient wires every route before it enables an input", "[midi][devices]")
{
    auto owned = std::make_unique<ProbeBackend>();
    auto* probe = owned.get();

    MidiInputClient client (std::move (owned));
    probe->client = &client;

    // 1 Hz, not a real rate: the collectors' retime is relative to the clock
    // they were seeded with at the top of rebuild, and events below
    // (elapsed - (numSamples << 5)) samples are dropped at drain time. Scaling
    // the whole conversion by the sample rate makes that window
    // (512 << 5) / 1 Hz = over four hours wide, so the delivery assertion below
    // measures routing rather than how long this process took to reach it.
    client.rebuild (1.0);

    REQUIRE (probe->enableCount == 2);
    REQUIRE (probe->routeReady);
    REQUIRE (probe->bankBuilt);

    dusk::MidiBuffer out;
    out.reserveBytes (256);

    // Both events were routed from inside enable(), so each landing in its own
    // collector is what proves the route existed before the enable.
    for (int i = 0; i < 2; ++i)
    {
        client.drainBlock (i, out, 512);
        REQUIRE (countProbeNotes (out) == 1);
    }

    // The synthetic slot is bound to no backend source, so nothing routes there.
    client.drainBlock (client.getVirtualKeyboardIndex(), out, 512);
    REQUIRE (out.isEmpty());
}

TEST_CASE ("MidiInputClient rebuild fences the backend's dispatch", "[midi][devices]")
{
    auto owned = std::make_unique<ProbeBackend>();
    auto* probe = owned.get();

    MidiInputClient client (std::move (owned));
    client.rebuild (48000.0);
    client.attachCallback();
    REQUIRE (probe->dispatching);

    const int stopsBefore = probe->stopCount;
    client.rebuild (48000.0);

    // Stopped for the mutation, restored after it - a caller that skipped the
    // detach fence still never has the MIDI thread reading a half-built bank.
    REQUIRE (probe->stopCount == stopsBefore + 1);
    REQUIRE (probe->dispatching);
    REQUIRE (! probe->enabledWhileDispatching);

    client.detachCallback();
    REQUIRE (! probe->dispatching);

    // A rebuild with dispatch already stopped leaves it stopped.
    client.rebuild (48000.0);
    REQUIRE (! probe->dispatching);
}

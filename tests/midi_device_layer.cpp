#include <catch2/catch_test_macros.hpp>

#include "engine/midi/MidiBackend.h"
#include "engine/midi/MidiDevices.h"
#include "foundation/MidiBuffer.h"
#include "support/ThreadWakeups.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

using duskstudio::midi::MidiInputClient;
using duskstudio::midi::MidiOutputBank;
using duskstudio::test::eventually;
using duskstudio::test::returnsPromptly;
using duskstudio::test::staysAt;

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

    SECTION ("Hardware isolation retains slots and the virtual keyboard, then restores subscriptions")
    {
        const int enabledBefore = probe->enableCount;
        REQUIRE (client.setHardwareInputsEnabledForTest (false));
        client.disableAllDevices();
        client.rebuild (1.0);
        REQUIRE (probe->enableCount == enabledBefore);
        REQUIRE (client.getNumInputs() == 3);
        REQUIRE (client.resolveIndex ("probe:a") == 0);
        REQUIRE (client.resolveIndex ("probe:b") == 1);
        REQUIRE (client.getVirtualKeyboardIndex() == 2);

        dusk::MidiBuffer out;
        out.reserveBytes (256);
        for (int input = 0; input < 2; ++input)
        {
            client.drainBlock (input, out, 512);
            REQUIRE (out.isEmpty());
        }
        client.postVirtualKeyboardMidi (kProbeNote, 3);
        client.drainBlock (client.getVirtualKeyboardIndex(), out, 512);
        REQUIRE (countProbeNotes (out) == 1);
        REQUIRE_FALSE (client.setHardwareInputsEnabledForTest (false));
        REQUIRE_FALSE (client.setHardwareInputsEnabledForTest (true));

        client.disableAllDevices();
        client.rebuild (1.0);
        REQUIRE (probe->enableCount == enabledBefore + 2);
        for (int input = 0; input < 2; ++input)
        {
            client.drainBlock (input, out, 512);
            REQUIRE (countProbeNotes (out) == 1);
        }
        REQUIRE (client.setHardwareInputsEnabledForTest (true));
    }
}

namespace
{
// An output backend that, like the ALSA client after a hard error, drops every
// route when a send fails.
class RetiringOutputBackend final : public duskstudio::midi::IMidiOutputBackend
{
public:
    std::vector<duskstudio::midi::BackendDeviceInfo> enumerate() override
    {
        return { { "Retiring A", "retiring:a" }, { "Retiring B", "retiring:b" } };
    }

    std::string migrateIdentifier (const std::string&) override { return {}; }

    bool open (const std::string& identifier) override
    {
        ++opens;
        if (refuseOpen) return false;
        routes.insert (identifier);
        return true;
    }

    void closeAll() override { routes.clear(); }
    bool isOpen (const std::string& identifier) const override { return routes.count (identifier) > 0; }

    bool send (const std::string& identifier, int, const dusk::MidiBuffer&, double, double) override
    {
        if (! isOpen (identifier)) return false;
        if (failures > 0)
        {
            --failures;
            routes.clear();
            return false;
        }
        ++delivered[identifier];
        return true;
    }

    std::set<std::string> routes;
    std::map<std::string, int> delivered;
    int failures = 0;
    int opens = 0;
    bool refuseOpen = false;
};

dusk::MidiBuffer probeNote()
{
    dusk::MidiBuffer buffer;
    REQUIRE (buffer.addEvent (kProbeNote, 3, 0));
    return buffer;
}
} // namespace

TEST_CASE ("MidiOutputBank reopens the routes a failed send made its backend drop", "[midi][devices]")
{
    auto owned = std::make_unique<RetiringOutputBackend>();
    auto* backend = owned.get();
    MidiOutputBank bank;
    bank.installBackend (std::move (owned));
    REQUIRE (bank.ensureOpen (0));
    REQUIRE (bank.ensureOpen (1));

    backend->failures = 1;
    CHECK_FALSE (bank.send (0, probeNote()));
    CHECK (backend->isOpen ("retiring:a"));
    CHECK (backend->isOpen ("retiring:b"));
    CHECK (bank.isOpen (0));
    CHECK (bank.isOpen (1));
    REQUIRE (bank.send (0, probeNote()));
    REQUIRE (bank.send (1, probeNote()));
    CHECK (backend->delivered["retiring:a"] == 1);
    CHECK (backend->delivered["retiring:b"] == 1);

    SECTION ("a success between failures earns another reopen")
    {
        backend->failures = 1;
        CHECK_FALSE (bank.send (1, probeNote()));
        REQUIRE (bank.send (1, probeNote()));
        CHECK (backend->delivered["retiring:b"] == 2);
    }

    SECTION ("a backend that fails again before any success is given up")
    {
        backend->failures = 2;
        CHECK_FALSE (bank.send (0, probeNote()));
        CHECK_FALSE (bank.send (0, probeNote()));
        CHECK_FALSE (bank.isOpen (0));
        CHECK_FALSE (bank.isOpen (1));
        REQUIRE (bank.ensureOpen (0));
        REQUIRE (bank.send (0, probeNote()));
        CHECK (backend->delivered["retiring:a"] == 2);
    }

    SECTION ("a route that cannot be reopened stops being offered")
    {
        backend->failures = 1;
        backend->refuseOpen = true;
        CHECK_FALSE (bank.send (0, probeNote()));
        CHECK_FALSE (bank.isOpen (0));
        CHECK_FALSE (bank.isOpen (1));
    }
}

namespace
{
// One output port, observed from the test thread while the pump sends to it.
class LoggingOutputBackend final : public duskstudio::midi::IMidiOutputBackend
{
public:
    std::vector<duskstudio::midi::BackendDeviceInfo> enumerate() override
    {
        return { { "Logging", "logging:0" } };
    }

    std::string migrateIdentifier (const std::string&) override { return {}; }

    bool open (const std::string&) override
    {
        if (refuseOpen.load()) return false;
        routeOpen.store (true);
        return true;
    }

    void closeAll() override { routeOpen.store (false); }
    bool isOpen (const std::string&) const override { return routeOpen.load(); }

    bool send (const std::string&, int, const dusk::MidiBuffer& events, double baseTimeMs, double) override
    {
        if (! routeOpen.load()) return false;
        if (failNextSend.exchange (false))
        {
            if (duringFailedSend) duringFailedSend();
            routeOpen.store (false);
            return false;
        }
        const std::lock_guard<std::mutex> lock (mutex);
        for (const auto meta : events)
        {
            notes.push_back (meta.data[1]);
            baseTimes.push_back (baseTimeMs);
        }
        return true;
    }

    std::vector<int> delivered() const
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return notes;
    }

    // The time each delivered note's block is sent from, in delivered() order.
    std::vector<double> deliveredBaseTimesMs() const
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return baseTimes;
    }

    std::atomic<bool> routeOpen { false }, refuseOpen { false }, failNextSend { false };
    // Pump thread, inside the send that fails. Set before startPump.
    std::function<void()> duringFailedSend;

private:
    mutable std::mutex mutex;
    std::vector<int> notes;
    std::vector<double> baseTimes;
};

dusk::MidiBuffer noteBlock (int note)
{
    dusk::MidiBuffer buffer;
    buffer.reserveBytes (64);
    const std::uint8_t bytes[3] { 0x90, (std::uint8_t) note, 100 };
    REQUIRE (buffer.addEvent (bytes, 3, 0));
    return buffer;
}

LoggingOutputBackend& installLogging (MidiOutputBank& bank)
{
    auto owned = std::make_unique<LoggingOutputBackend>();
    auto& backend = *owned;
    bank.installBackend (std::move (owned));
    return backend;
}
} // namespace

TEST_CASE ("The MIDI output pump makes no timer wake-ups while no output is open", "[midi][devices]")
{
    std::atomic<int> idle { 0 };
    MidiOutputBank bank;
    installLogging (bank);
    bank.setPumpIdleHookForTest ([&] { idle.fetch_add (1); });
    bank.startPump();

    REQUIRE (eventually ([&] { return idle.load() > 0; }));
    CHECK (staysAt (idle, idle.load()));
    bank.stopPump();
}

TEST_CASE ("An output opened while the MIDI output pump sleeps gets what is queued for it", "[midi][devices]")
{
    std::atomic<int> idle { 0 };
    MidiOutputBank bank;
    auto& backend = installLogging (bank);
    bank.setPumpIdleHookForTest ([&] { idle.fetch_add (1); });
    bank.startPump();
    REQUIRE (eventually ([&] { return idle.load() > 0; }));

    REQUIRE (bank.ensureOpen (0));
    bank.queueRt (0, 0, noteBlock (60), 48000.0);
    CHECK (eventually ([&] { return backend.delivered() == std::vector<int> { 60 }; }));
    bank.stopPump();
}

// The open lands after the pump has seen no output open and before it waits,
// and nothing else wakes it: the open alone has to end that wait.
TEST_CASE ("An output opened as the MIDI output pump goes to sleep is not missed", "[midi][devices]")
{
    std::atomic<bool> armed { true }, opened { false };
    MidiOutputBank bank;
    auto& backend = installLogging (bank);
    bank.setPumpIdleHookForTest ([&]
    {
        if (! armed.exchange (false)) return;
        opened.store (bank.ensureOpen (0));
        bank.queueRt (0, 0, noteBlock (61), 48000.0);
    });
    bank.startPump();

    CHECK (eventually ([&] { return backend.delivered() == std::vector<int> { 61 }; }));
    CHECK (opened.load());
    bank.stopPump();
}

TEST_CASE ("Closing the last MIDI output puts the output pump back to sleep", "[midi][devices]")
{
    std::atomic<int> idle { 0 };
    MidiOutputBank bank;
    auto& backend = installLogging (bank);
    REQUIRE (bank.ensureOpen (0));
    bank.setPumpIdleHookForTest ([&] { idle.fetch_add (1); });
    bank.startPump();

    // Nothing signals the pump for a queued block, so this one going out is
    // the open-port poll at work.
    bank.queueRt (0, 0, noteBlock (62), 48000.0);
    REQUIRE (eventually ([&] { return backend.delivered() == std::vector<int> { 62 }; }));

    const int beforeClose = idle.load();
    bank.closeAll();
    REQUIRE (eventually ([&] { return idle.load() > beforeClose; }));
    CHECK (staysAt (idle, idle.load()));

    REQUIRE (bank.ensureOpen (0));
    bank.queueRt (0, 0, noteBlock (63), 48000.0);
    CHECK (eventually ([&] { return backend.delivered() == std::vector<int> { 62, 63 }; }));
    bank.stopPump();
}

// A failed send whose port cannot be reopened leaves no output open, so the
// pump goes to sleep. A block the audio thread committed against that port
// while the send was failing must not wait out the sleep and reach the port
// once it is opened again.
TEST_CASE ("A block queued for an output the pump gave up on is not sent when it reopens", "[midi][devices]")
{
    std::atomic<int> idle { 0 };
    MidiOutputBank bank;
    auto& backend = installLogging (bank);
    REQUIRE (bank.ensureOpen (0));
    bank.setPumpIdleHookForTest ([&] { idle.fetch_add (1); });
    backend.duringFailedSend = [&] { bank.queueRt (0, 0, noteBlock (65), 48000.0); };
    bank.startPump();

    // A block out first spends the open's wake-up in a timed wait, so nothing
    // wakes the pump between the failed send and the reopen.
    bank.queueRt (0, 0, noteBlock (63), 48000.0);
    REQUIRE (eventually ([&] { return backend.delivered() == std::vector<int> { 63 }; }));

    backend.refuseOpen.store (true);
    backend.failNextSend.store (true);
    bank.queueRt (0, 0, noteBlock (64), 48000.0);
    REQUIRE (eventually ([&] { return ! bank.isOpen (0) && idle.load() > 0; }));

    backend.refuseOpen.store (false);
    REQUIRE (bank.ensureOpen (0));
    bank.queueRt (0, 0, noteBlock (66), 48000.0);
    REQUIRE (eventually ([&] { return backend.delivered().size() > 1; }));
    CHECK (backend.delivered() == std::vector<int> { 63, 66 });
    bank.stopPump();
}

TEST_CASE ("Stopping the MIDI output pump wakes it from its sleep", "[midi][devices]")
{
    auto bank = std::make_unique<MidiOutputBank>();
    installLogging (*bank);
    auto idle = std::make_shared<std::atomic<int>> (0);
    bank->setPumpIdleHookForTest ([idle] { idle->fetch_add (1); });
    bank->startPump();
    REQUIRE (eventually ([&] { return idle->load() > 0; }));

    auto* stopping = bank.get();
    const bool stopped = returnsPromptly ([stopping] { stopping->stopPump(); });
    // A stop that never returns still holds the bank.
    if (! stopped) (void) bank.release();
    REQUIRE (stopped);
}

// The blocks of a device cycle that is handed over in parts are all queued
// when the cycle starts. The last of four 512-sample blocks at 48 kHz plays
// 32 ms after the first, and has to be sent from there.
TEST_CASE ("MIDI queued from a later block of a split cycle leaves at that block's time",
           "[midi][devices]")
{
    MidiOutputBank bank;
    auto& backend = installLogging (bank);
    bank.startPump();
    REQUIRE (bank.ensureOpen (0));

    bank.queueRt (0, 0, noteBlock (60), 48000.0, 0);
    bank.queueRt (0, 0, noteBlock (61), 48000.0, 3 * 512);
    REQUIRE (eventually ([&] { return backend.delivered() == std::vector<int> { 60, 61 }; }));
    bank.stopPump();

    const auto times = backend.deliveredBaseTimesMs();
    REQUIRE (times.size() == 2);
    // The second was queued no earlier than the first, so its base is at least
    // its 1536 samples later.
    CHECK (times[1] - times[0] >= 32.0);
}


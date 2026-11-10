#include <catch2/catch_test_macros.hpp>

#include "engine/midi/AlsaSeqMidi.h"
#include "engine/midi/MidiDevices.h"
#include "foundation/MidiBuffer.h"

#include <alsa/asoundlib.h>

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
// Keep real hot-plug announcements, but isolate negative callback assertions
// from other processes. The raw ProbeClient still proves positive notification.
std::atomic<bool> onlyTestProcessClients { false };

struct IsolatedSeqClients
{
    IsolatedSeqClients() { onlyTestProcessClients.store (true); }
    ~IsolatedSeqClients() { onlyTestProcessClients.store (false); }
};
}

extern "C" int __real_snd_seq_query_next_client (snd_seq_t*, snd_seq_client_info_t*);

extern "C" int __wrap_snd_seq_query_next_client (snd_seq_t* seq, snd_seq_client_info_t* info)
{
    int result = 0;
    do
    {
        result = __real_snd_seq_query_next_client (seq, info);
    }
    while (result >= 0 && onlyTestProcessClients.load()
           && snd_seq_client_info_get_pid (info) != static_cast<int> (getpid()));
    return result;
}

using namespace duskstudio::midi;

namespace
{
// First enumerated identifier whose display name contains portSubstr. The tests
// match a full "client: port" display name to avoid selecting some other
// vendor's MIDI port.
std::string findId (const std::vector<BackendDeviceInfo>& devs, const std::string& portSubstr)
{
    for (auto& d : devs)
        if (d.name.find (portSubstr) != std::string::npos)
            return d.identifier;
    return {};
}

// Raw "<client>-<port>" sequencer address of the named port, which is exactly
// the identifier format JUCE minted on Linux and therefore what pre-existing
// sessions carry.
std::string legacyAddressOf (const std::string& clientName, const std::string& portName)
{
    snd_seq_t* seq = nullptr;
    if (snd_seq_open (&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0)
        return {};

    std::string found;
    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t*   pinfo;
    snd_seq_client_info_alloca (&cinfo);
    snd_seq_port_info_alloca (&pinfo);

    snd_seq_client_info_set_client (cinfo, -1);
    while (found.empty() && snd_seq_query_next_client (seq, cinfo) >= 0)
    {
        const int client = snd_seq_client_info_get_client (cinfo);
        if (clientName != snd_seq_client_info_get_name (cinfo)) continue;

        snd_seq_port_info_set_client (pinfo, client);
        snd_seq_port_info_set_port (pinfo, -1);
        while (snd_seq_query_next_port (seq, pinfo) >= 0)
        {
            if (portName != snd_seq_port_info_get_name (pinfo)) continue;
            found = std::to_string (client) + "-" + std::to_string (snd_seq_port_info_get_port (pinfo));
            break;
        }
    }
    snd_seq_close (seq);
    return found;
}

int indexOfName (const std::vector<MidiDeviceInfo>& devs, const std::string& portSubstr)
{
    for (int i = 0; i < (int) devs.size(); ++i)
        if (devs[(size_t) i].name.find (portSubstr) != std::string::npos)
            return i;
    return -1;
}

// Receiver-side collector: the poll thread pushes decoded messages; the test
// waits on the condition variable instead of sleeping/polling.
struct Sink
{
    std::mutex                            m;
    std::condition_variable               cv;
    std::vector<std::vector<std::uint8_t>> messages;
    std::string                           lastId;

    void onMidi (const std::string& id, const std::uint8_t* bytes, int n)
    {
        std::lock_guard<std::mutex> lk (m);
        lastId = id;
        messages.emplace_back (bytes, bytes + n);
        cv.notify_all();
    }

    bool waitFor (std::size_t want, int ms = 2000)
    {
        std::unique_lock<std::mutex> lk (m);
        return cv.wait_for (lk, std::chrono::milliseconds (ms),
                            [&] { return messages.size() >= want; });
    }
};

// A MIDI client that comes and goes on demand - what a hot-plug looks like to
// the sequencer. Creating it raises CLIENT_START + PORT_START on the announce
// port; closing it raises PORT_EXIT + CLIENT_EXIT.
struct ProbeClient
{
    static constexpr const char* kClientName = "Dusk Hotplug Probe";
    static constexpr const char* kPortName   = "Probe Out";
    static constexpr const char* kDisplayName = "Dusk Hotplug Probe: Probe Out";

    snd_seq_t* seq = nullptr;

    bool appear()
    {
        if (snd_seq_open (&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) { seq = nullptr; return false; }
        snd_seq_set_client_name (seq, kClientName);
        return snd_seq_create_simple_port (seq, kPortName,
                   SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                   SND_SEQ_PORT_TYPE_MIDI_GENERIC) >= 0;
    }

    void vanish()
    {
        if (seq != nullptr) snd_seq_close (seq);
        seq = nullptr;
    }

    ~ProbeClient() { vanish(); }
};

// The backends never list each other, so a loopback goes through another
// client: one port that passes whatever reaches it on to whoever subscribes to
// it, as a MIDI thru box would.
struct Bridge
{
    static constexpr const char* kClientName  = "Dusk Loopback Bridge";
    static constexpr const char* kPortName    = "Through";
    static constexpr const char* kDisplayName = "Dusk Loopback Bridge: Through";

    snd_seq_t*        seq  = nullptr;
    int               port = -1;
    std::atomic<bool> running { false };
    std::thread       thread;

    bool open()
    {
        if (snd_seq_open (&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) { seq = nullptr; return false; }
        snd_seq_set_client_name (seq, kClientName);
        port = snd_seq_create_simple_port (seq, kPortName,
                   SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ
                       | SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                   SND_SEQ_PORT_TYPE_MIDI_GENERIC);
        if (port < 0) return false;
        running.store (true);
        thread = std::thread ([this] { forward(); });
        return true;
    }

    void forward()
    {
        const int count = snd_seq_poll_descriptors_count (seq, POLLIN);
        if (count <= 0) return;
        std::vector<pollfd> fds ((std::size_t) count);
        snd_seq_poll_descriptors (seq, fds.data(), (unsigned) count, POLLIN);
        while (running.load())
        {
            if (poll (fds.data(), (nfds_t) count, 20) <= 0) continue;
            snd_seq_event_t* ev = nullptr;
            while (snd_seq_event_input (seq, &ev) >= 0 && ev != nullptr)
            {
                if (ev->type >= SND_SEQ_EVENT_CLIENT_START && ev->type <= SND_SEQ_EVENT_PORT_UNSUBSCRIBED)
                    continue;
                snd_seq_ev_set_source (ev, (unsigned char) port);
                snd_seq_ev_set_subs (ev);
                snd_seq_ev_set_direct (ev);
                snd_seq_event_output_direct (seq, ev);
            }
        }
    }

    ~Bridge()
    {
        running.store (false);
        if (thread.joinable()) thread.join();
        if (seq != nullptr) snd_seq_close (seq);
    }
};

// Device-change reports, counted off the poll thread.
struct ChangeCounter
{
    std::mutex              m;
    std::condition_variable cv;
    int                     count = 0;

    void bump()
    {
        std::lock_guard<std::mutex> lk (m);
        ++count;
        cv.notify_all();
    }

    int total()
    {
        std::lock_guard<std::mutex> lk (m);
        return count;
    }

    bool waitFor (int want, int ms = 2000)
    {
        std::unique_lock<std::mutex> lk (m);
        return cv.wait_for (lk, std::chrono::milliseconds (ms), [&] { return count >= want; });
    }
};

// A successful snd_seq_open is not enough: a constrained CI sequencer accepts
// the handle yet cannot create or route ports, so the loopback would fail
// rather than skip. Probe an actual send -> receive round-trip and only run the
// assertions where it completes end to end.
bool loopbackAvailable()
{
    snd_seq_t* probe = nullptr;
    if (snd_seq_open (&probe, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0)
        return false;
    snd_seq_close (probe);

    Sink              sink;
    Bridge            bridge;
    if (! bridge.open()) return false;
    AlsaSeqMidiInput  in;
    AlsaSeqMidiOutput out;

    const std::string src = findId (in.enumerate(),  Bridge::kDisplayName);
    const std::string dst = findId (out.enumerate(), Bridge::kDisplayName);
    if (src.empty() || dst.empty())     return false;
    if (! in.enable (src) || ! out.open (dst)) return false;

    in.setReceiver ([&] (const std::string& id, const std::uint8_t* b, int n, double)
                    { sink.onMidi (id, b, n); });
    in.start();
    dusk::MidiBuffer buf;
    const std::uint8_t note[] { 0x90, 60, 100 };
    buf.addEvent (note, 3, 0);
    out.send (dst, 0, buf, 0.0, 48000.0);
    const bool ok = sink.waitFor (1);
    in.stop();
    return ok;
}
} // namespace

TEST_CASE ("ALSA seq backend enumerates its loopback partner and stable ids", "[midi][alsa]")
{
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    Bridge bridge;
    REQUIRE (bridge.open());
    AlsaSeqMidiInput  in;
    AlsaSeqMidiOutput out;

    // The bridge's port is both a MIDI source to the input backend and a
    // destination to the output backend.
    REQUIRE_FALSE (findId (in.enumerate(),  Bridge::kDisplayName).empty());
    REQUIRE_FALSE (findId (out.enumerate(), Bridge::kDisplayName).empty());

    SECTION ("the loopback endpoint keeps a stable, scheme-prefixed identifier")
    {
        // Compare this test's own bridge endpoint across two enumerations
        // rather than the whole global list, which other MIDI clients on the
        // machine can reorder or resize between calls.
        const std::string a = findId (out.enumerate(), Bridge::kDisplayName);
        const std::string b = findId (out.enumerate(), Bridge::kDisplayName);
        REQUIRE_FALSE (a.empty());
        REQUIRE (a == b);
        REQUIRE (a.rfind ("alsa-seq:", 0) == 0);
    }
}

TEST_CASE ("ALSA seq backend migrates a legacy address identifier", "[midi][alsa]")
{
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    Bridge bridge;
    REQUIRE (bridge.open());
    AlsaSeqMidiInput  in;
    AlsaSeqMidiOutput out;

    const std::string current = findId (out.enumerate(), Bridge::kDisplayName);
    const std::string legacy  = legacyAddressOf (Bridge::kClientName, Bridge::kPortName);
    REQUIRE_FALSE (current.empty());
    REQUIRE_FALSE (legacy.empty());

    // A session saved under the old scheme resolves back to the same port.
    REQUIRE (out.migrateIdentifier (legacy) == current);

    // Addresses that no longer point anywhere, and anything not in the legacy
    // shape at all, report a miss rather than guessing.
    REQUIRE (out.migrateIdentifier ("9999-0").empty());
    REQUIRE (out.migrateIdentifier ("").empty());
    REQUIRE (out.migrateIdentifier ("-").empty());
    REQUIRE (out.migrateIdentifier ("12").empty());
    REQUIRE (out.migrateIdentifier ("12-").empty());
    REQUIRE (out.migrateIdentifier ("x-0").empty());
    REQUIRE (out.migrateIdentifier ("12-0junk").empty());
    REQUIRE (out.migrateIdentifier (current).empty());   // already migrated

    REQUIRE (in.migrateIdentifier (legacy) == findId (in.enumerate(), Bridge::kDisplayName));
}

TEST_CASE ("MidiOutputBank RT queue delivers through the pump, bounded by its depth",
           "[midi][alsa]")
{
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    Sink sink;
    Bridge bridge;
    REQUIRE (bridge.open());
    AlsaSeqMidiInput in;
    in.setReceiver ([&] (const std::string& id, const std::uint8_t* b, int n, double)
                    { sink.onMidi (id, b, n); });

    // Built after the bridge so its port is in the enumeration. The bank owns
    // its own output backend, which reaches the input through the bridge.
    MidiOutputBank bank;
    bank.rebuild();

    const int dst = indexOfName (bank.getDevices(), Bridge::kDisplayName);
    REQUIRE (dst >= 0);

    const std::string src = findId (in.enumerate(), Bridge::kDisplayName);
    REQUIRE_FALSE (src.empty());
    REQUIRE (in.enable (src));
    in.start();

    auto oneNote = [] (int note)
    {
        dusk::MidiBuffer b;
        b.reserveBytes (64);
        const std::uint8_t bytes[3] { 0x90, (std::uint8_t) (note & 0x7f), 100 };
        b.addEvent (bytes, 3, 0);
        return b;
    };

    // A closed port takes no slot: this must not consume queue depth, so all
    // kQueueDepth of the notes below still commit.
    REQUIRE_FALSE (bank.isOpen (dst));
    for (int i = 0; i < MidiOutputBank::kQueueDepth; ++i)
        bank.queueRt (dst, 0, oneNote (0x7f), 48000.0);

    REQUIRE (bank.ensureOpen (dst));
    REQUIRE (bank.isOpen (dst));

    // Past the slot cap: dropped whole rather than truncated, and likewise takes
    // no slot. Filled to exactly what a slot holds and then one event beyond,
    // keyed off the shared ceiling so raising that cannot quietly turn this into
    // a block that fits.
    dusk::MidiBuffer big;
    big.reserveBytes (dusk::kMidiRoutingBlockBytes * 2);
    dusk::MidiBuffer slotSized;
    slotSized.reserveBytes (dusk::kMidiRoutingBlockBytes);
    const std::uint8_t filler[3] { 0x90, 0x7f, 100 };
    int n = 0;
    for (; slotSized.addEvent (filler, 3, n); ++n)
        REQUIRE (big.addEvent (filler, 3, n));
    REQUIRE (big.addEvent (filler, 3, n));
    bank.queueRt (dst, 0, big, 48000.0);

    // The pump is not running, so only kQueueDepth blocks can commit; the rest
    // drop. Each carries its own note number, so the delivered set identifies
    // exactly which ones survived.
    constexpr int kPushed = MidiOutputBank::kQueueDepth * 3;
    for (int i = 0; i < kPushed; ++i)
        bank.queueRt (dst, 0, oneNote (i), 48000.0);

    bank.startPump();
    REQUIRE (sink.waitFor ((std::size_t) MidiOutputBank::kQueueDepth));
    // Nothing beyond the queue's depth is ever delivered - neither the dropped
    // blocks nor the over-cap one.
    REQUIRE_FALSE (sink.waitFor ((std::size_t) MidiOutputBank::kQueueDepth + 1, 250));

    bank.stopPump();

    {
        std::lock_guard<std::mutex> lk (sink.m);
        REQUIRE (sink.messages.size() == (std::size_t) MidiOutputBank::kQueueDepth);
        std::vector<int> deliveredNotes;
        for (const auto& message : sink.messages)
            deliveredNotes.push_back (message.size() > 1 ? message[1] : -1);
        CAPTURE (deliveredNotes);
        for (int i = 0; i < (int) sink.messages.size(); ++i)
        {
            REQUIRE (sink.messages[(size_t) i].size() == 3);
            REQUIRE (sink.messages[(size_t) i][1] == (std::uint8_t) i);   // FIFO order, first kQueueDepth pushed
        }
        sink.messages.clear();
    }

    const auto consumeCleanup = [&]
    {
        REQUIRE (sink.waitFor (2));
        std::lock_guard<std::mutex> lk (sink.m);
        REQUIRE (sink.messages.size() == 2);
        const std::vector<std::uint8_t> soundOff { 0xb0, 120, 0 };
        const std::vector<std::uint8_t> notesOff { 0xb0, 123, 0 };
        REQUIRE (sink.messages[0] == soundOff);
        REQUIRE (sink.messages[1] == notesOff);
        sink.messages.clear();
    };

    SECTION ("a close discards what was queued before it, even if the port reopens")
    {
        bank.queueRt (dst, 0, oneNote (0x11), 48000.0);
        bank.closeAll();
        REQUIRE_FALSE (bank.isOpen (dst));
        consumeCleanup();

        // Reopened before the pump ever ran: the block from before the close
        // must be gone, not delivered late to the reopened port.
        REQUIRE (bank.ensureOpen (dst));
        bank.startPump();
        REQUIRE_FALSE (sink.waitFor (1, 250));

        // Delivery still works for anything queued after the reopen.
        bank.queueRt (dst, 0, oneNote (0x22), 48000.0);
        REQUIRE (sink.waitFor (1));
        bank.stopPump();

        std::lock_guard<std::mutex> lk (sink.m);
        REQUIRE (sink.messages.size() == 1);
        REQUIRE (sink.messages[0][1] == 0x22);
    }

    SECTION ("a re-enumeration discards what was queued against the old order")
    {
        bank.queueRt (dst, 0, oneNote (0x33), 48000.0);
        bank.rebuild();
        consumeCleanup();

        // Indices are minted afresh, so the pre-rebuild block must not be sent
        // even though the same physical port is still there and reopens.
        const int again = indexOfName (bank.getDevices(), Bridge::kDisplayName);
        REQUIRE (again >= 0);
        REQUIRE (bank.ensureOpen (again));
        bank.startPump();
        REQUIRE_FALSE (sink.waitFor (1, 250));

        bank.queueRt (again, 0, oneNote (0x44), 48000.0);
        REQUIRE (sink.waitFor (1));
        bank.stopPump();

        std::lock_guard<std::mutex> lk (sink.m);
        REQUIRE (sink.messages.size() == 1);
        REQUIRE (sink.messages[0][1] == 0x44);
    }

    in.stop();
}

TEST_CASE ("ALSA seq identifier migration is safe while the poll thread runs",
           "[midi][alsa]")
{
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    // A session load resolves saved identifiers WITHOUT stopping MIDI input
    // (AudioEngine::reresolveTrackMidiFromSession is deliberately detach-free),
    // so migrateIdentifier queries the same sequencer handle the poll thread is
    // reading. Drive both at once: the assertions below hold either way, but
    // this is the shape a sanitiser build needs in order to see the conflict.
    Sink sink;
    Bridge bridge;
    REQUIRE (bridge.open());
    AlsaSeqMidiInput  in;
    AlsaSeqMidiOutput out;

    const std::string bridgeAsSource = findId (in.enumerate(),  Bridge::kDisplayName);
    const std::string bridgeAsDest   = findId (out.enumerate(), Bridge::kDisplayName);
    REQUIRE_FALSE (bridgeAsSource.empty());
    REQUIRE_FALSE (bridgeAsDest.empty());
    REQUIRE (in.enable (bridgeAsSource));
    REQUIRE (out.open (bridgeAsDest));

    in.setReceiver ([&] (const std::string& id, const std::uint8_t* b, int n, double)
                    { sink.onMidi (id, b, n); });
    in.start();

    // The migration has to run on the INPUT backend: that is the one whose poll
    // thread is reading the handle being queried. Migrating on the output side
    // would query a handle nothing else touches and prove nothing.
    const std::string legacy = legacyAddressOf (Bridge::kClientName, Bridge::kPortName);
    REQUIRE_FALSE (legacy.empty());

    dusk::MidiBuffer note;
    const std::uint8_t bytes[3] { 0x90, 60, 100 };
    note.addEvent (bytes, 3, 0);

    constexpr int kRounds = 64;
    for (int i = 0; i < kRounds; ++i)
    {
        // The send keeps the poll thread busy on the same handle the migration
        // walks.
        REQUIRE (out.send (bridgeAsDest, 0, note, 0.0, 48000.0));
        REQUIRE (in.migrateIdentifier (legacy) == bridgeAsSource);
    }

    REQUIRE (sink.waitFor (kRounds));
    in.stop();
    out.closeAll();
}

TEST_CASE ("ALSA seq MIDI loopback round-trips bytes exactly", "[midi][alsa]")
{
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    Sink sink;
    Bridge bridge;
    REQUIRE (bridge.open());
    AlsaSeqMidiInput  in;
    AlsaSeqMidiOutput out;
    in.setReceiver ([&] (const std::string& id, const std::uint8_t* b, int n, double)
                    { sink.onMidi (id, b, n); });

    const std::string bridgeAsSource = findId (in.enumerate(),  Bridge::kDisplayName);
    const std::string bridgeAsDest   = findId (out.enumerate(), Bridge::kDisplayName);
    REQUIRE_FALSE (bridgeAsSource.empty());
    REQUIRE_FALSE (bridgeAsDest.empty());

    REQUIRE (in.enable (bridgeAsSource));   // in  <- bridge  (subscription)
    REQUIRE (out.open (bridgeAsDest));      // out -> bridge  (subscription)
    REQUIRE (out.isOpen (bridgeAsDest));
    in.start();

    SECTION ("three-byte channel messages")
    {
        dusk::MidiBuffer buf;
        const std::uint8_t noteOn[]  { 0x90, 60, 100 };
        const std::uint8_t noteOff[] { 0x80, 60, 0 };
        REQUIRE (buf.addEvent (noteOn,  3, 0));
        REQUIRE (buf.addEvent (noteOff, 3, 0));
        REQUIRE (out.send (bridgeAsDest, 0, buf, 0.0, 48000.0));

        REQUIRE (sink.waitFor (2));
        std::lock_guard<std::mutex> lk (sink.m);
        REQUIRE (sink.messages.size() == 2);
        REQUIRE (sink.messages[0] == std::vector<std::uint8_t> ({ 0x90, 60, 100 }));
        REQUIRE (sink.messages[1] == std::vector<std::uint8_t> ({ 0x80, 60, 0 }));
        REQUIRE (sink.lastId == bridgeAsSource);   // demuxed back to the source id
    }

    SECTION ("running-status burst expands to full messages")
    {
        // One wire-format run of three note-ons sharing a status byte. The
        // encoder must expand it to three events; the decoder must restore each
        // status byte (no_status).
        dusk::MidiBuffer buf;
        const std::uint8_t burst[] { 0x90, 60, 100, 62, 101, 64, 102 };
        REQUIRE (buf.addEvent (burst, (int) sizeof burst, 0));
        REQUIRE (out.send (bridgeAsDest, 0, buf, 0.0, 48000.0));

        REQUIRE (sink.waitFor (3));
        std::lock_guard<std::mutex> lk (sink.m);
        REQUIRE (sink.messages.size() == 3);
        REQUIRE (sink.messages[0] == std::vector<std::uint8_t> ({ 0x90, 60, 100 }));
        REQUIRE (sink.messages[1] == std::vector<std::uint8_t> ({ 0x90, 62, 101 }));
        REQUIRE (sink.messages[2] == std::vector<std::uint8_t> ({ 0x90, 64, 102 }));
    }

    SECTION ("multi-byte sysex round-trips intact")
    {
        std::vector<std::uint8_t> sysex;
        sysex.push_back (0xF0);
        sysex.push_back (0x7D);              // non-commercial / test manufacturer id
        for (int i = 0; i < 64; ++i)
            sysex.push_back ((std::uint8_t) (i & 0x7f));
        sysex.push_back (0xF7);

        dusk::MidiBuffer buf;
        REQUIRE (buf.addEvent (sysex.data(), (int) sysex.size(), 0));
        REQUIRE (out.send (bridgeAsDest, 0, buf, 0.0, 48000.0));

        REQUIRE (sink.waitFor (1));
        std::lock_guard<std::mutex> lk (sink.m);
        REQUIRE (sink.messages.size() == 1);
        REQUIRE (sink.messages[0] == sysex);
    }

    in.stop();
    out.closeAll();
}

TEST_CASE ("ALSA seq kernel queue delivers a producer's blocks in the order it sent them",
           "[midi][alsa]")
{
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    Sink sink;
    Bridge bridge;
    REQUIRE (bridge.open());
    AlsaSeqMidiInput  in;
    AlsaSeqMidiOutput out;
    in.setReceiver ([&] (const std::string& id, const std::uint8_t* b, int n, double)
                    { sink.onMidi (id, b, n); });

    const std::string bridgeAsSource = findId (in.enumerate(),  Bridge::kDisplayName);
    const std::string bridgeAsDest   = findId (out.enumerate(), Bridge::kDisplayName);
    REQUIRE_FALSE (bridgeAsSource.empty());
    REQUIRE_FALSE (bridgeAsDest.empty());
    REQUIRE (in.enable (bridgeAsSource));
    REQUIRE (out.open (bridgeAsDest));
    in.start();

    // The second block's clock reading came back behind the first's, so its
    // note-off at offset 0 is nominally due before the note-on at offset 500.
    const std::vector<std::uint8_t> on { 0x90, 60, 100 };
    const std::vector<std::uint8_t> off { 0x80, 60, 0 };
    dusk::MidiBuffer first, second;
    REQUIRE (first.addEvent (on.data(), 3, 500));
    REQUIRE (second.addEvent (off.data(), 3, 0));
    const double base = backendClockMs() + 30.0;
    REQUIRE (out.send (bridgeAsDest, 0, first, base, 48000.0));
    REQUIRE (out.send (bridgeAsDest, 0, second, base - 5.0, 48000.0));

    REQUIRE (sink.waitFor (2));
    {
        std::lock_guard<std::mutex> lk (sink.m);
        REQUIRE (sink.messages.size() == 2);
        CHECK (sink.messages[0] == on);
        CHECK (sink.messages[1] == off);
    }

    in.stop();
    out.closeAll();
}

TEST_CASE ("ALSA seq backend reports a port appearing and disappearing", "[midi][alsa]")
{
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    ChangeCounter changes;
    AlsaSeqMidiInput in;
    in.setDeviceChangeHandler ([&] { changes.bump(); });
    in.start();

    ProbeClient probe;
    REQUIRE (probe.appear());
    REQUIRE (changes.waitFor (1));

    // The report is only worth anything if re-enumerating on the back of it
    // actually finds the new port - that is all the engine does when it fires.
    REQUIRE_FALSE (findId (in.enumerate(), ProbeClient::kDisplayName).empty());

    const int afterAppear = changes.total();
    probe.vanish();
    REQUIRE (changes.waitFor (afterAppear + 1));
    REQUIRE (findId (in.enumerate(), ProbeClient::kDisplayName).empty());

    in.stop();
}

TEST_CASE ("ALSA seq backend does not report its own subscriptions as a change",
           "[midi][alsa]")
{
    const IsolatedSeqClients isolatedClients;
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    // enable() subscribes, and the sequencer announces every subscription. Were
    // those counted, each refresh would announce the next one and the engine
    // would rebuild for ever.
    //
    // The bridge is opened FIRST so its port already exists when the input
    // subscribes to the announce port - otherwise its arrival is itself a
    // genuine port-set change, queued and delivered mid-test.
    Bridge bridge;
    REQUIRE (bridge.open());
    AlsaSeqMidiInput in;

    const std::string src = findId (in.enumerate(), Bridge::kDisplayName);
    REQUIRE_FALSE (src.empty());

    ChangeCounter changes;
    in.setDeviceChangeHandler ([&] { changes.bump(); });
    in.start();

    for (int i = 0; i < 8; ++i)
    {
        REQUIRE (in.enable (src));
        in.disableAll();
    }

    REQUIRE_FALSE (changes.waitFor (1, 250));

    in.stop();
}

namespace
{
// Another copy of Dusk Studio running alongside, as the sequencer shows it: a
// client of the same name with the same two ports.
struct OtherCopy
{
    snd_seq_t* seq     = nullptr;
    int        inPort  = -1;
    int        outPort = -1;

    bool open()
    {
        if (snd_seq_open (&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) { seq = nullptr; return false; }
        snd_seq_set_client_name (seq, "Dusk Studio");
        inPort  = snd_seq_create_simple_port (seq, "MIDI In",
                      SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        outPort = snd_seq_create_simple_port (seq, "MIDI Out",
                      SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        return inPort >= 0 && outPort >= 0;
    }

    int client() const { return snd_seq_client_id (seq); }
    std::string address (int port) const { return std::to_string (client()) + "-" + std::to_string (port); }

    ~OtherCopy() { if (seq != nullptr) snd_seq_close (seq); }
};

struct SeqPort
{
    int         client = 0;
    int         port   = 0;
    int         pid    = -1;
    std::string clientName;
    std::string portName;

    std::string address() const { return std::to_string (client) + "-" + std::to_string (port); }
};

std::vector<SeqPort> allPorts()
{
    std::vector<SeqPort> out;
    snd_seq_t* seq = nullptr;
    if (snd_seq_open (&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0)
        return out;

    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t*   pinfo;
    snd_seq_client_info_alloca (&cinfo);
    snd_seq_port_info_alloca (&pinfo);

    snd_seq_client_info_set_client (cinfo, -1);
    while (snd_seq_query_next_client (seq, cinfo) >= 0)
    {
        const int client = snd_seq_client_info_get_client (cinfo);
        snd_seq_port_info_set_client (pinfo, client);
        snd_seq_port_info_set_port (pinfo, -1);
        while (snd_seq_query_next_port (seq, pinfo) >= 0)
            out.push_back ({ client, snd_seq_port_info_get_port (pinfo), snd_seq_client_info_get_pid (cinfo),
                             snd_seq_client_info_get_name (cinfo), snd_seq_port_info_get_name (pinfo) });
    }
    snd_seq_close (seq);
    return out;
}

int countNamed (const std::vector<BackendDeviceInfo>& devs, const std::string& name)
{
    int n = 0;
    for (const auto& d : devs)
        n += d.name == name ? 1 : 0;
    return n;
}
} // namespace

TEST_CASE ("ALSA seq backends list neither of this process's own clients", "[midi][alsa]")
{
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    // The input and the output are two sequencer clients, both called "Dusk
    // Studio". Listed, either one routes Dusk Studio's MIDI output back into its
    // own input.
    AlsaSeqMidiInput  in;
    AlsaSeqMidiOutput out;
    OtherCopy copy;
    REQUIRE (copy.open());

    std::vector<std::string> own;
    int foreignIns = 0, foreignOuts = 0;
    for (const auto& p : allPorts())
    {
        if (p.clientName != "Dusk Studio") continue;
        if (p.pid == (int) getpid() && p.client != copy.client())
            own.push_back (p.address());
        else
        {
            foreignIns  += p.portName == "MIDI In"  ? 1 : 0;
            foreignOuts += p.portName == "MIDI Out" ? 1 : 0;
        }
    }
    REQUIRE (own.size() == 2);

    for (const auto& address : own)
    {
        CAPTURE (address);
        CHECK (in.migrateIdentifier (address).empty());
        CHECK (out.migrateIdentifier (address).empty());
    }
    CHECK (countNamed (in.enumerate(),  "Dusk Studio: MIDI Out") == foreignOuts);
    CHECK (countNamed (out.enumerate(), "Dusk Studio: MIDI In")  == foreignIns);

    CHECK_FALSE (in.migrateIdentifier (copy.address (copy.outPort)).empty());
    CHECK_FALSE (out.migrateIdentifier (copy.address (copy.inPort)).empty());
}

TEST_CASE ("ALSA seq backend does not report this process's own clients coming and going",
           "[midi][alsa]")
{
    const IsolatedSeqClients isolatedClients;
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    // Replacing the output backend closes this process's output client and a
    // new one opens. Neither was ever in the list, so neither may arm a
    // rebuild that renumbers the inputs.
    ChangeCounter changes;
    AlsaSeqMidiInput in;
    (void) in.enumerate();
    in.setDeviceChangeHandler ([&] { changes.bump(); });
    in.start();

    for (int i = 0; i < 4; ++i)
    {
        {
            AlsaSeqMidiOutput out;
            (void) out.enumerate();
            REQUIRE (changes.total() == 0);
        }
        REQUIRE (changes.total() == 0);
    }
    REQUIRE_FALSE (changes.waitFor (1, 250));

    ProbeClient probe;
    REQUIRE (probe.appear());
    REQUIRE (changes.waitFor (1));

    in.stop();
}

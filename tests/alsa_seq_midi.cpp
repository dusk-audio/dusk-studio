#include <catch2/catch_test_macros.hpp>

#include "engine/midi/AlsaSeqMidi.h"
#include "engine/midi/MidiDevices.h"
#include "foundation/MidiBuffer.h"

#include <alsa/asoundlib.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

using namespace duskstudio::midi;

namespace
{
// First enumerated identifier whose display name contains portSubstr. Both
// backends register under client "Dusk Studio", so the tests match the full
// "Dusk Studio: MIDI ..." display name to avoid selecting some other vendor's
// MIDI port.
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
    AlsaSeqMidiInput  in;
    AlsaSeqMidiOutput out;

    const std::string src = findId (in.enumerate(),  "Dusk Studio: MIDI Out");
    const std::string dst = findId (out.enumerate(), "Dusk Studio: MIDI In");
    if (src.empty() || dst.empty())     return false;
    if (! in.enable (src) || ! out.open (dst)) return false;

    in.setReceiver ([&] (const std::string& id, const std::uint8_t* b, int n, double)
                    { sink.onMidi (id, b, n); });
    in.start();
    dusk::MidiBuffer buf;
    const std::uint8_t note[] { 0x90, 60, 100 };
    buf.addEvent (note, 3, 0);
    out.send (dst, buf, 0.0, 48000.0);
    const bool ok = sink.waitFor (1);
    in.stop();
    return ok;
}
} // namespace

TEST_CASE ("ALSA seq backend enumerates its loopback partner and stable ids", "[midi][alsa]")
{
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    AlsaSeqMidiInput  in;
    AlsaSeqMidiOutput out;

    // The output backend's port is a MIDI source to the input backend; the input
    // backend's port is a destination to the output backend.
    REQUIRE_FALSE (findId (in.enumerate(),  "Dusk Studio: MIDI Out").empty());
    REQUIRE_FALSE (findId (out.enumerate(), "Dusk Studio: MIDI In").empty());

    SECTION ("the loopback endpoint keeps a stable, scheme-prefixed identifier")
    {
        // Compare this test's own "Dusk Studio: MIDI In" endpoint across two enumerations
        // rather than the whole global list, which other MIDI clients on the
        // machine can reorder or resize between calls.
        const std::string a = findId (out.enumerate(), "Dusk Studio: MIDI In");
        const std::string b = findId (out.enumerate(), "Dusk Studio: MIDI In");
        REQUIRE_FALSE (a.empty());
        REQUIRE (a == b);
        REQUIRE (a.rfind ("alsa-seq:", 0) == 0);
    }
}

TEST_CASE ("ALSA seq backend migrates a legacy address identifier", "[midi][alsa]")
{
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    AlsaSeqMidiInput  in;
    AlsaSeqMidiOutput out;

    const std::string current = findId (out.enumerate(), "Dusk Studio: MIDI In");
    const std::string legacy  = legacyAddressOf ("Dusk Studio", "MIDI In");
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

    REQUIRE (in.migrateIdentifier (legacyAddressOf ("Dusk Studio", "MIDI Out"))
             == findId (in.enumerate(), "Dusk Studio: MIDI Out"));
}

TEST_CASE ("MidiOutputBank RT queue delivers through the pump, bounded by its depth",
           "[midi][alsa]")
{
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    Sink sink;
    AlsaSeqMidiInput in;
    in.setReceiver ([&] (const std::string& id, const std::uint8_t* b, int n, double)
                    { sink.onMidi (id, b, n); });

    // Built after the input backend so its port is in the enumeration. The bank
    // owns its own output backend, which the input sees as a source.
    MidiOutputBank bank;
    bank.rebuild();

    const int dst = indexOfName (bank.getDevices(), "Dusk Studio: MIDI In");
    REQUIRE (dst >= 0);

    const std::string src = findId (in.enumerate(), "Dusk Studio: MIDI Out");
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
        bank.queueRt (dst, oneNote (0x7f), 48000.0);

    REQUIRE (bank.ensureOpen (dst));
    REQUIRE (bank.isOpen (dst));

    // Past the slot cap: dropped whole rather than truncated, and likewise takes
    // no slot.
    dusk::MidiBuffer big;
    big.reserveBytes (1 << 16);
    const std::uint8_t filler[3] { 0x90, 0x7f, 100 };
    for (int i = 0; i < 4096; ++i)
        REQUIRE (big.addEvent (filler, 3, i));
    bank.queueRt (dst, big, 48000.0);

    // The pump is not running, so only kQueueDepth blocks can commit; the rest
    // drop. Each carries its own note number, so the delivered set identifies
    // exactly which ones survived.
    constexpr int kPushed = MidiOutputBank::kQueueDepth * 3;
    for (int i = 0; i < kPushed; ++i)
        bank.queueRt (dst, oneNote (i), 48000.0);

    bank.startPump();
    REQUIRE (sink.waitFor ((std::size_t) MidiOutputBank::kQueueDepth));
    // Nothing beyond the queue's depth is ever delivered - neither the dropped
    // blocks nor the over-cap one.
    REQUIRE_FALSE (sink.waitFor ((std::size_t) MidiOutputBank::kQueueDepth + 1, 250));

    bank.stopPump();

    {
        std::lock_guard<std::mutex> lk (sink.m);
        REQUIRE (sink.messages.size() == (std::size_t) MidiOutputBank::kQueueDepth);
        for (int i = 0; i < (int) sink.messages.size(); ++i)
        {
            REQUIRE (sink.messages[(size_t) i].size() == 3);
            REQUIRE (sink.messages[(size_t) i][1] == (std::uint8_t) i);   // FIFO order, first kQueueDepth pushed
        }
        sink.messages.clear();
    }

    SECTION ("a close discards what was queued before it, even if the port reopens")
    {
        bank.queueRt (dst, oneNote (0x11), 48000.0);
        bank.closeAll();
        REQUIRE_FALSE (bank.isOpen (dst));

        // Reopened before the pump ever ran: the block from before the close
        // must be gone, not delivered late to the reopened port.
        REQUIRE (bank.ensureOpen (dst));
        bank.startPump();
        REQUIRE_FALSE (sink.waitFor (1, 250));

        // Delivery still works for anything queued after the reopen.
        bank.queueRt (dst, oneNote (0x22), 48000.0);
        REQUIRE (sink.waitFor (1));
        bank.stopPump();

        std::lock_guard<std::mutex> lk (sink.m);
        REQUIRE (sink.messages.size() == 1);
        REQUIRE (sink.messages[0][1] == 0x22);
    }

    SECTION ("a re-enumeration discards what was queued against the old order")
    {
        bank.queueRt (dst, oneNote (0x33), 48000.0);
        bank.rebuild();

        // Indices are minted afresh, so the pre-rebuild block must not be sent
        // even though the same physical port is still there and reopens.
        const int again = indexOfName (bank.getDevices(), "Dusk Studio: MIDI In");
        REQUIRE (again >= 0);
        REQUIRE (bank.ensureOpen (again));
        bank.startPump();
        REQUIRE_FALSE (sink.waitFor (1, 250));

        bank.queueRt (again, oneNote (0x44), 48000.0);
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
    AlsaSeqMidiInput  in;
    AlsaSeqMidiOutput out;

    const std::string outAsSource = findId (in.enumerate(),  "Dusk Studio: MIDI Out");
    const std::string inAsDest    = findId (out.enumerate(), "Dusk Studio: MIDI In");
    REQUIRE_FALSE (outAsSource.empty());
    REQUIRE_FALSE (inAsDest.empty());
    REQUIRE (in.enable (outAsSource));
    REQUIRE (out.open (inAsDest));

    in.setReceiver ([&] (const std::string& id, const std::uint8_t* b, int n, double)
                    { sink.onMidi (id, b, n); });
    in.start();

    // The migration has to run on the INPUT backend: that is the one whose poll
    // thread is reading the handle being queried. Migrating on the output side
    // would query a handle nothing else touches and prove nothing.
    const std::string legacy = legacyAddressOf ("Dusk Studio", "MIDI Out");
    REQUIRE_FALSE (legacy.empty());

    dusk::MidiBuffer note;
    const std::uint8_t bytes[3] { 0x90, 60, 100 };
    note.addEvent (bytes, 3, 0);

    constexpr int kRounds = 64;
    for (int i = 0; i < kRounds; ++i)
    {
        // The send keeps the poll thread busy on the same handle the migration
        // walks.
        REQUIRE (out.send (inAsDest, note, 0.0, 48000.0));
        REQUIRE (in.migrateIdentifier (legacy) == outAsSource);
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
    AlsaSeqMidiInput  in;
    AlsaSeqMidiOutput out;
    in.setReceiver ([&] (const std::string& id, const std::uint8_t* b, int n, double)
                    { sink.onMidi (id, b, n); });

    const std::string outAsSource = findId (in.enumerate(),  "Dusk Studio: MIDI Out");
    const std::string inAsDest    = findId (out.enumerate(), "Dusk Studio: MIDI In");
    REQUIRE_FALSE (outAsSource.empty());
    REQUIRE_FALSE (inAsDest.empty());

    REQUIRE (in.enable (outAsSource));   // in  <- out  (subscription)
    REQUIRE (out.open (inAsDest));       // out ->  in  (subscription)
    REQUIRE (out.isOpen (inAsDest));
    in.start();

    SECTION ("three-byte channel messages")
    {
        dusk::MidiBuffer buf;
        const std::uint8_t noteOn[]  { 0x90, 60, 100 };
        const std::uint8_t noteOff[] { 0x80, 60, 0 };
        REQUIRE (buf.addEvent (noteOn,  3, 0));
        REQUIRE (buf.addEvent (noteOff, 3, 0));
        REQUIRE (out.send (inAsDest, buf, 0.0, 48000.0));

        REQUIRE (sink.waitFor (2));
        std::lock_guard<std::mutex> lk (sink.m);
        REQUIRE (sink.messages.size() == 2);
        REQUIRE (sink.messages[0] == std::vector<std::uint8_t> ({ 0x90, 60, 100 }));
        REQUIRE (sink.messages[1] == std::vector<std::uint8_t> ({ 0x80, 60, 0 }));
        REQUIRE (sink.lastId == outAsSource);   // demuxed back to the source id
    }

    SECTION ("running-status burst expands to full messages")
    {
        // One wire-format run of three note-ons sharing a status byte. The
        // encoder must expand it to three events; the decoder must restore each
        // status byte (no_status).
        dusk::MidiBuffer buf;
        const std::uint8_t burst[] { 0x90, 60, 100, 62, 101, 64, 102 };
        REQUIRE (buf.addEvent (burst, (int) sizeof burst, 0));
        REQUIRE (out.send (inAsDest, buf, 0.0, 48000.0));

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
        REQUIRE (out.send (inAsDest, buf, 0.0, 48000.0));

        REQUIRE (sink.waitFor (1));
        std::lock_guard<std::mutex> lk (sink.m);
        REQUIRE (sink.messages.size() == 1);
        REQUIRE (sink.messages[0] == sysex);
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
    if (! loopbackAvailable())
        SKIP ("ALSA sequencer loopback unavailable - headless CI");

    // enable() subscribes, and the sequencer announces every subscription. Were
    // those counted, each refresh would announce the next one and the engine
    // would rebuild for ever.
    //
    // The output backend is constructed FIRST so its port already exists when
    // the input subscribes to the announce port - otherwise its arrival is
    // itself a genuine port-set change, queued and delivered mid-test.
    AlsaSeqMidiOutput out;
    AlsaSeqMidiInput  in;

    const std::string src = findId (in.enumerate(), "Dusk Studio: MIDI Out");
    REQUIRE_FALSE (src.empty());

    ChangeCounter changes;
    in.setDeviceChangeHandler ([&] { changes.bump(); });
    in.start();

    for (int i = 0; i < 8; ++i)
    {
        REQUIRE (in.enable (src));
        in.disableAll();
    }

    // Nothing came or went, so nothing may be reported. A failure here means
    // either subscribe traffic is being counted or some other MIDI client on the
    // machine appeared during the window.
    REQUIRE_FALSE (changes.waitFor (1, 250));

    in.stop();
}

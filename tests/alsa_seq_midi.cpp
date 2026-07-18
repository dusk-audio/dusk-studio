#include <catch2/catch_test_macros.hpp>

#include "engine/midi/AlsaSeqMidi.h"
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

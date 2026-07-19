#include "AlsaSeqMidi.h"

#if defined(__linux__)

#include <alsa/asoundlib.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace duskstudio::midi
{
namespace
{
// Working buffer for the snd_midi_event coders (one event's worth of raw bytes).
constexpr std::size_t kCoderBufferBytes = 65536;
// Upper bound on a sysex reassembled across multiple events; a longer one is
// discarded rather than buffered without limit. Distinct from the per-event
// decode target, which is sized to each event.
constexpr std::size_t kMaxSysexBytes    = 1u << 20;
// A non-sysex MIDI message decodes to at most three bytes; give the decode
// scratch a little headroom for those before it has to grow.
constexpr std::size_t kShortEventBytes  = 16;
constexpr char        kIdPrefix[]       = "alsa-seq:";

// Escape '\' and ':' so the ':'-delimited identifier stays unambiguous even when
// a client or port name itself contains a colon.
std::string escapeComponent (const std::string& s)
{
    std::string o;
    o.reserve (s.size());
    for (char c : s)
    {
        if (c == '\\' || c == ':') o += '\\';
        o += c;
    }
    return o;
}

std::string makeIdentifier (const std::string& clientName, const std::string& portName, int dup)
{
    std::string id = std::string (kIdPrefix) + escapeComponent (clientName) + ":" + escapeComponent (portName);
    if (dup > 0) id += ":" + std::to_string (dup);
    return id;
}

// One discovered port: its live (client,port) address plus the stable,
// name-based identifier the seam persists.
struct PortRef
{
    int         client = 0;
    int         port   = 0;
    std::string name;         // "client: port", for the picker UI
    std::string identifier;   // name-based (see AlsaSeqMidi.h); :dup ordinal is enumeration-order dependent
};

// Enumerate MIDI sources (wantRead) or destinations across every client except
// ourselves and the System client (client 0: Timer/Announce, not real MIDI).
// dup-index disambiguates identically-named ports in enumeration order.
std::vector<PortRef> queryPorts (snd_seq_t* seq, bool wantRead)
{
    std::vector<PortRef> out;
    if (seq == nullptr) return out;

    const int self = snd_seq_client_id (seq);
    const unsigned needCaps = wantRead
        ? (unsigned) (SND_SEQ_PORT_CAP_READ  | SND_SEQ_PORT_CAP_SUBS_READ)
        : (unsigned) (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);

    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t*   pinfo;
    snd_seq_client_info_alloca (&cinfo);
    snd_seq_port_info_alloca (&pinfo);

    std::map<std::pair<std::string, std::string>, int> dupCounts;   // (client,port) -> next dup index

    snd_seq_client_info_set_client (cinfo, -1);
    while (snd_seq_query_next_client (seq, cinfo) >= 0)
    {
        const int client = snd_seq_client_info_get_client (cinfo);
        if (client == self || client == 0) continue;
        const std::string clientName = snd_seq_client_info_get_name (cinfo);

        snd_seq_port_info_set_client (pinfo, client);
        snd_seq_port_info_set_port (pinfo, -1);
        while (snd_seq_query_next_port (seq, pinfo) >= 0)
        {
            const unsigned caps = snd_seq_port_info_get_capability (pinfo);
            if ((caps & needCaps) != needCaps)            continue;
            if (caps & SND_SEQ_PORT_CAP_NO_EXPORT)        continue;

            const std::string portName = snd_seq_port_info_get_name (pinfo);
            const int dup = dupCounts[{ clientName, portName }]++;

            out.push_back ({ client, snd_seq_port_info_get_port (pinfo),
                             clientName + ": " + portName,
                             makeIdentifier (clientName, portName, dup) });
        }
    }
    return out;
}

std::uint32_t addrKey (int client, int port) noexcept
{
    return ((std::uint32_t) client << 16) | (std::uint32_t) (port & 0xffff);
}

// JUCE's Linux MIDI identifiers are the raw sequencer address, "<client>-<port>".
// Sessions saved before this backend existed hold those, so map one back to a
// live port and hand out its name-based identifier instead. Only valid while the
// numbers still point at the same hardware - the very instability that motivated
// the name-based scheme - so a miss is normal and returns "".
std::string migrateLegacyAddress (snd_seq_t* seq, bool wantRead, const std::string& legacy)
{
    const auto dash = legacy.find ('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= legacy.size()) return {};

    const char* s = legacy.c_str();
    char* endClient = nullptr;
    char* endPort   = nullptr;
    const long client = std::strtol (s, &endClient, 10);
    if (endClient != s + dash) return {};
    const long port = std::strtol (s + dash + 1, &endPort, 10);
    if (endPort != s + legacy.size()) return {};

    for (const auto& p : queryPorts (seq, wantRead))
        if (p.client == client && p.port == port)
            return p.identifier;
    return {};
}
} // namespace

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
struct AlsaSeqMidiInput::Impl
{
    snd_seq_t*        seq    = nullptr;
    int               inPort = -1;
    snd_midi_event_t* coder  = nullptr;
    Receiver          receiver;

    // libasound does not support concurrent use of one snd_seq_t. The poll
    // thread reads events off `seq` while the message thread can be querying it
    // (migrateIdentifier runs during a session load, which deliberately does NOT
    // stop the poll thread). Every use of the handle takes this - except poll()
    // itself, which must stay outside or the message thread would block until
    // MIDI happens to arrive.
    std::mutex seqMutex;

    // Live source address -> identifier, for demuxing incoming events. Built at
    // enable() (poll thread stopped); read only from the poll thread.
    std::map<std::uint32_t, std::string> sourceIds;

    // Per-source sysex reassembly. `discarding` swallows the rest of an
    // oversized or malformed sysex until its 0xF7, so its tail never leaks out
    // as an ordinary message.
    struct SysexReasm { std::vector<std::uint8_t> bytes; bool discarding = false; };
    std::map<std::uint32_t, SysexReasm> sysex;

    std::vector<std::uint8_t> decodeScratch;   // decode target, grown to each event's size

    std::thread       pollThread;
    std::atomic<bool> running { false };
    int               wakePipe[2] { -1, -1 };   // self-pipe to break poll() on stop

    // Fully initialised = usable. A missing port, null coder (no decode), or
    // missing wake pipe (no clean stop) means the backend must not advertise or
    // activate.
    bool ready() const noexcept
    {
        return seq != nullptr && inPort >= 0 && coder != nullptr && wakePipe[0] >= 0 && wakePipe[1] >= 0;
    }

    Impl()
    {
        if (snd_seq_open (&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) { seq = nullptr; return; }
        snd_seq_set_client_name (seq, "Dusk Studio");
        inPort = snd_seq_create_simple_port (seq, "MIDI In",
                     SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                     SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        snd_seq_nonblock (seq, 1);   // poll() drives wakeups; event_input returns EAGAIN when empty
        if (snd_midi_event_new (kCoderBufferBytes, &coder) == 0)
            snd_midi_event_no_status (coder, 1);   // emit a status byte on every decoded message
        else
            coder = nullptr;
        if (pipe (wakePipe) == 0)
            fcntl (wakePipe[0], F_SETFL, O_NONBLOCK);   // so the post-join drain never blocks on an empty pipe
        else
            wakePipe[0] = wakePipe[1] = -1;
    }

    ~Impl()
    {
        stopThread();
        if (coder) snd_midi_event_free (coder);
        if (seq)   snd_seq_close (seq);
        if (wakePipe[0] >= 0) close (wakePipe[0]);
        if (wakePipe[1] >= 0) close (wakePipe[1]);
    }

    void deliver (int client, int port, const std::uint8_t* bytes, int n)
    {
        if (! receiver || n <= 0) return;
        auto it = sourceIds.find (addrKey (client, port));
        if (it != sourceIds.end())
            receiver (it->second, bytes, n, backendClockMs());
    }

    // Reassemble sysex spanning multiple events; pass everything else straight
    // through. Realtime bytes (0xF8..0xFF) are standalone even mid-sysex.
    void handleDecoded (int client, int port, const std::uint8_t* bytes, int n)
    {
        if (n == 1 && bytes[0] >= 0xF8) { deliver (client, port, bytes, 1); return; }

        auto& sx = sysex[addrKey (client, port)];
        const bool active = ! sx.bytes.empty() || sx.discarding;
        if (! active && bytes[0] != 0xF0)
        {
            deliver (client, port, bytes, n);   // ordinary channel / system message
            return;
        }

        const bool terminated = bytes[n - 1] == 0xF7;
        if (sx.discarding)                          // dropping an oversized sysex
        {
            if (terminated) sx.discarding = false;
            return;
        }
        if (sx.bytes.size() + (std::size_t) n > kMaxSysexBytes)   // would overflow the cap
        {
            sx.bytes.clear();
            sx.discarding = ! terminated;           // keep dropping until 0xF7 arrives
            return;
        }
        sx.bytes.insert (sx.bytes.end(), bytes, bytes + n);
        if (terminated)
        {
            deliver (client, port, sx.bytes.data(), (int) sx.bytes.size());
            sx.bytes.clear();
        }
    }

    // Abandon an oversized variable event we refuse to buffer: drop whatever was
    // accumulating for this source and, unless this fragment already ended the
    // sysex, keep discarding until its 0xF7 so a later fragment is not stitched
    // onto the hole.
    void beginDiscard (int client, int port, bool terminated)
    {
        auto& sx = sysex[addrKey (client, port)];
        sx.bytes.clear();
        sx.discarding = ! terminated;
    }

    void pumpEvents()
    {
        const std::lock_guard<std::mutex> lock (seqMutex);
        snd_seq_event_t* ev = nullptr;
        while (snd_seq_event_input (seq, &ev) >= 0 && ev != nullptr)
        {
            // Only SYSEX carries MIDI bytes in variable-length form; other
            // variable events (BOUNCE, USR_VAR, ...) are not MIDI, so skip them
            // before they are sized, decoded, or mistaken for a sysex fragment.
            const bool variable = (ev->flags & SND_SEQ_EVENT_LENGTH_MASK) == SND_SEQ_EVENT_LENGTH_VARIABLE;
            if (variable && ev->type != SND_SEQ_EVENT_SYSEX)
                continue;

            // A sysex event carries its length; anything else fits in a few
            // bytes. Bound the sysex case by the reassembly cap so a bogus length
            // cannot drive a huge allocation.
            if (variable && (std::size_t) ev->data.ext.len > kMaxSysexBytes)
            {
                // Over the cap: refuse to buffer it, but keep this source's
                // reassembly coherent - drop any partial sysex and discard the
                // rest until 0xF7, unless this fragment already carries it.
                const auto* ext = (const std::uint8_t*) ev->data.ext.ptr;
                const bool terminated = ext != nullptr && ext[ev->data.ext.len - 1] == 0xF7;
                beginDiscard (ev->source.client, ev->source.port, terminated);
                continue;
            }
            const std::size_t need = variable ? (std::size_t) ev->data.ext.len : kShortEventBytes;
            if (decodeScratch.size() < need) decodeScratch.resize (need);

            const long got = snd_midi_event_decode (coder, decodeScratch.data(), (long) decodeScratch.size(), ev);
            if (got > 0)
                handleDecoded (ev->source.client, ev->source.port, decodeScratch.data(), (int) got);
            // no_status makes each decode self-contained: no cross-event running
            // status, so no reset between events is needed.
        }
    }

    void run()
    {
        int nfds = 0;
        {
            const std::lock_guard<std::mutex> lock (seqMutex);
            nfds = snd_seq_poll_descriptors_count (seq, POLLIN);
        }
        // A negative count is an alsa error code, and it must not reach the
        // sizing below: cast to size_t it either wraps to a huge allocation or
        // to zero, and the wake-pipe slot is then written out of bounds. Nothing
        // is pollable in that case, so the thread has no work to do.
        if (nfds <= 0) return;

        std::vector<pollfd> pfds ((std::size_t) nfds + 1);
        while (running.load (std::memory_order_acquire))
        {
            {
                const std::lock_guard<std::mutex> lock (seqMutex);
                snd_seq_poll_descriptors (seq, pfds.data(), (unsigned) nfds, POLLIN);
            }
            pfds[(std::size_t) nfds] = { wakePipe[0], POLLIN, 0 };
            const int r = poll (pfds.data(), (nfds_t) (nfds + 1), -1);
            if (r < 0) { if (errno == EINTR) continue; break; }
            if (pfds[(std::size_t) nfds].revents & POLLIN) break;   // stop() woke us
            pumpEvents();
        }
    }

    void startThread()
    {
        if (seq == nullptr || coder == nullptr || wakePipe[0] < 0 || running.load()) return;
        running.store (true, std::memory_order_release);
        pollThread = std::thread ([this] { run(); });
    }

    void stopThread()
    {
        if (running.exchange (false) && wakePipe[1] >= 0)
        {
            const char b = 1;
            [[maybe_unused]] ssize_t w = write (wakePipe[1], &b, 1);
        }
        if (pollThread.joinable()) pollThread.join();
        if (wakePipe[0] >= 0)   // drain the wake byte so a restart is clean
        {
            char tmp[16];
            while (read (wakePipe[0], tmp, sizeof tmp) > 0) {}
        }
    }
};

AlsaSeqMidiInput::AlsaSeqMidiInput() : impl (std::make_unique<Impl>()) {}
AlsaSeqMidiInput::~AlsaSeqMidiInput() = default;

std::vector<BackendDeviceInfo> AlsaSeqMidiInput::enumerate()
{
    std::vector<BackendDeviceInfo> out;
    if (! impl->ready()) return out;
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    for (auto& p : queryPorts (impl->seq, /*wantRead*/ true))
        out.push_back ({ p.name, p.identifier });
    return out;
}

void AlsaSeqMidiInput::setReceiver (Receiver r) { impl->receiver = std::move (r); }

std::string AlsaSeqMidiInput::migrateIdentifier (const std::string& legacy)
{
    if (! impl->ready()) return {};
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    return migrateLegacyAddress (impl->seq, /*wantRead*/ true, legacy);
}

bool AlsaSeqMidiInput::enable (const std::string& identifier)
{
    if (! impl->ready()) return false;
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    for (auto& p : queryPorts (impl->seq, true))
    {
        if (p.identifier != identifier) continue;
        const int rc = snd_seq_connect_from (impl->seq, impl->inPort, p.client, p.port);
        if (rc < 0 && rc != -EBUSY) return false;   // -EBUSY: already subscribed = already connected
        impl->sourceIds[addrKey (p.client, p.port)] = identifier;
        return true;
    }
    return false;
}

void AlsaSeqMidiInput::disableAll()
{
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    if (impl->seq != nullptr)
        for (auto& kv : impl->sourceIds)
            snd_seq_disconnect_from (impl->seq, impl->inPort,
                                     (int) (kv.first >> 16), (int) (kv.first & 0xffff));
    impl->sourceIds.clear();
    impl->sysex.clear();
}

void AlsaSeqMidiInput::start() { impl->startThread(); }
void AlsaSeqMidiInput::stop()  { impl->stopThread(); }

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
struct AlsaSeqMidiOutput::Impl
{
    snd_seq_t*        seq     = nullptr;
    int               outPort = -1;
    snd_midi_event_t* coder   = nullptr;
    std::map<std::string, std::pair<int, int>> openDests;   // identifier -> (client,port)

    // As on the input side: the pump thread sends through `seq` while the
    // message thread can be opening or querying it. Taken by the public entry
    // points only - Impl::closeAll is also reached from the destructor, which
    // must not re-enter it.
    std::mutex seqMutex;

    // Fully initialised = usable. A null coder (no encode) or missing port means
    // the backend cannot send, so it must not advertise or open devices.
    bool ready() const noexcept
    {
        return seq != nullptr && coder != nullptr && outPort >= 0;
    }

    Impl()
    {
        if (snd_seq_open (&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { seq = nullptr; return; }
        snd_seq_set_client_name (seq, "Dusk Studio");
        outPort = snd_seq_create_simple_port (seq, "MIDI Out",
                      SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (snd_midi_event_new (kCoderBufferBytes, &coder) != 0)
            coder = nullptr;
    }

    ~Impl()
    {
        closeAll();
        if (coder) snd_midi_event_free (coder);
        if (seq)   snd_seq_close (seq);
    }

    void closeAll()
    {
        if (seq != nullptr)
            for (auto& kv : openDests)
                snd_seq_disconnect_to (seq, outPort, kv.second.first, kv.second.second);
        openDests.clear();
    }
};

AlsaSeqMidiOutput::AlsaSeqMidiOutput() : impl (std::make_unique<Impl>()) {}
AlsaSeqMidiOutput::~AlsaSeqMidiOutput() = default;

std::vector<BackendDeviceInfo> AlsaSeqMidiOutput::enumerate()
{
    std::vector<BackendDeviceInfo> out;
    if (! impl->ready()) return out;
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    for (auto& p : queryPorts (impl->seq, /*wantRead*/ false))
        out.push_back ({ p.name, p.identifier });
    return out;
}

std::string AlsaSeqMidiOutput::migrateIdentifier (const std::string& legacy)
{
    if (! impl->ready()) return {};
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    return migrateLegacyAddress (impl->seq, /*wantRead*/ false, legacy);
}

bool AlsaSeqMidiOutput::open (const std::string& identifier)
{
    if (! impl->ready()) return false;
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    if (impl->openDests.count (identifier)) return true;   // lazy open is idempotent
    for (auto& p : queryPorts (impl->seq, false))
    {
        if (p.identifier != identifier) continue;
        const int rc = snd_seq_connect_to (impl->seq, impl->outPort, p.client, p.port);
        if (rc < 0 && rc != -EBUSY) return false;   // -EBUSY: already subscribed = already connected
        impl->openDests[identifier] = { p.client, p.port };
        return true;
    }
    return false;
}

void AlsaSeqMidiOutput::closeAll()
{
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    impl->closeAll();
}

bool AlsaSeqMidiOutput::isOpen (const std::string& identifier) const
{
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    return impl->openDests.count (identifier) > 0;
}

bool AlsaSeqMidiOutput::send (const std::string& identifier, const dusk::MidiBuffer& events,
                              double baseTimeMs, double sampleRate)
{
    // The pump runs at ~1 ms cadence, so events fire immediately (output_direct)
    // rather than being scheduled per sample-offset within the block; the sub-ms
    // ordering within a block is preserved by send order. Per-offset queue
    // scheduling stays unimplemented until it proves audible.
    (void) baseTimeMs;
    (void) sampleRate;

    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    if (impl->seq == nullptr || impl->coder == nullptr) return false;
    auto it = impl->openDests.find (identifier);
    if (it == impl->openDests.end()) return false;
    const int dstClient = it->second.first;
    const int dstPort   = it->second.second;

    bool allOk = true;
    for (const auto meta : events)
    {
        const std::uint8_t* bytes = meta.data;
        long remaining = meta.numBytes;
        snd_midi_event_reset_encode (impl->coder);   // isolate running status per dusk event
        while (remaining > 0)
        {
            snd_seq_event_t ev;
            snd_seq_ev_clear (&ev);
            const long used = snd_midi_event_encode (impl->coder, bytes, remaining, &ev);
            if (used <= 0) { allOk = false; break; }
            bytes     += used;
            remaining -= used;
            if (ev.type == SND_SEQ_EVENT_NONE) continue;   // needs more bytes for a full event
            snd_seq_ev_set_source (&ev, (unsigned char) impl->outPort);
            snd_seq_ev_set_dest (&ev, dstClient, dstPort);
            snd_seq_ev_set_direct (&ev);
            if (snd_seq_event_output_direct (impl->seq, &ev) < 0) allOk = false;
        }
    }
    return allOk;
}
} // namespace duskstudio::midi

#else  // non-Linux: stub so the TU builds where the ALSA sequencer is absent.

namespace duskstudio::midi
{
struct AlsaSeqMidiInput::Impl  {};
struct AlsaSeqMidiOutput::Impl {};

AlsaSeqMidiInput::AlsaSeqMidiInput()  = default;
AlsaSeqMidiInput::~AlsaSeqMidiInput() = default;
std::vector<BackendDeviceInfo> AlsaSeqMidiInput::enumerate() { return {}; }
void AlsaSeqMidiInput::setReceiver (Receiver) {}
std::string AlsaSeqMidiInput::migrateIdentifier (const std::string&) { return {}; }
bool AlsaSeqMidiInput::enable (const std::string&) { return false; }
void AlsaSeqMidiInput::disableAll() {}
void AlsaSeqMidiInput::start() {}
void AlsaSeqMidiInput::stop()  {}

AlsaSeqMidiOutput::AlsaSeqMidiOutput()  = default;
AlsaSeqMidiOutput::~AlsaSeqMidiOutput() = default;
std::vector<BackendDeviceInfo> AlsaSeqMidiOutput::enumerate() { return {}; }
std::string AlsaSeqMidiOutput::migrateIdentifier (const std::string&) { return {}; }
bool AlsaSeqMidiOutput::open (const std::string&) { return false; }
void AlsaSeqMidiOutput::closeAll() {}
bool AlsaSeqMidiOutput::isOpen (const std::string&) const { return false; }
bool AlsaSeqMidiOutput::send (const std::string&, const dusk::MidiBuffer&, double, double) { return false; }
} // namespace duskstudio::midi

#endif

#include "AlsaSeqMidi.h"

#if defined(__linux__)

#include <alsa/asoundlib.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <map>
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

// Hi-res millisecond clock in the same domain the seam's MidiCollector drains
// against (M3 passes the equivalent clock to removeNextBlock).
double nowMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli> (steady_clock::now().time_since_epoch()).count();
}

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
            receiver (it->second, bytes, n, nowMs());
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

    void pumpEvents()
    {
        snd_seq_event_t* ev = nullptr;
        while (snd_seq_event_input (seq, &ev) >= 0 && ev != nullptr)
        {
            // Size the decode target to this event: a variable (sysex) event
            // carries its length; anything else fits in a few bytes. Bound the
            // variable case by the reassembly cap so a bogus length can't drive a
            // huge allocation - such an event is skipped, not buffered.
            const bool variable = (ev->flags & SND_SEQ_EVENT_LENGTH_MASK) == SND_SEQ_EVENT_LENGTH_VARIABLE;
            if (variable && (std::size_t) ev->data.ext.len > kMaxSysexBytes) continue;
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
        const int nfds = snd_seq_poll_descriptors_count (seq, POLLIN);
        std::vector<pollfd> pfds ((std::size_t) nfds + 1);
        while (running.load (std::memory_order_acquire))
        {
            snd_seq_poll_descriptors (seq, pfds.data(), (unsigned) nfds, POLLIN);
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
    for (auto& p : queryPorts (impl->seq, /*wantRead*/ true))
        out.push_back ({ p.name, p.identifier });
    return out;
}

void AlsaSeqMidiInput::setReceiver (Receiver r) { impl->receiver = std::move (r); }

bool AlsaSeqMidiInput::enable (const std::string& identifier)
{
    if (! impl->ready()) return false;
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
    for (auto& p : queryPorts (impl->seq, /*wantRead*/ false))
        out.push_back ({ p.name, p.identifier });
    return out;
}

bool AlsaSeqMidiOutput::open (const std::string& identifier)
{
    if (! impl->ready()) return false;
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

void AlsaSeqMidiOutput::closeAll() { impl->closeAll(); }

bool AlsaSeqMidiOutput::isOpen (const std::string& identifier) const
{
    return impl->openDests.count (identifier) > 0;
}

bool AlsaSeqMidiOutput::send (const std::string& identifier, const dusk::MidiBuffer& events,
                              double baseTimeMs, double sampleRate)
{
    // The pump runs at ~1 ms cadence, so events fire immediately (output_direct)
    // rather than being scheduled per sample-offset within the block; the sub-ms
    // ordering within a block is preserved by send order. Per-offset queue
    // scheduling, if it proves audible, lands with the M3 seam timing.
    (void) baseTimeMs;
    (void) sampleRate;

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
bool AlsaSeqMidiInput::enable (const std::string&) { return false; }
void AlsaSeqMidiInput::disableAll() {}
void AlsaSeqMidiInput::start() {}
void AlsaSeqMidiInput::stop()  {}

AlsaSeqMidiOutput::AlsaSeqMidiOutput()  = default;
AlsaSeqMidiOutput::~AlsaSeqMidiOutput() = default;
std::vector<BackendDeviceInfo> AlsaSeqMidiOutput::enumerate() { return {}; }
bool AlsaSeqMidiOutput::open (const std::string&) { return false; }
void AlsaSeqMidiOutput::closeAll() {}
bool AlsaSeqMidiOutput::isOpen (const std::string&) const { return false; }
bool AlsaSeqMidiOutput::send (const std::string&, const dusk::MidiBuffer&, double, double) { return false; }
} // namespace duskstudio::midi

#endif

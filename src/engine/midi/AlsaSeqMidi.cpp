#include "AlsaSeqMidi.h"

#if defined(__linux__)

#include <alsa/asoundlib.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <limits>
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
// Scheduled variable events occupy kernel pool cells, including their payload.
constexpr std::size_t kOutputCoderBufferBytes = 4096;
constexpr std::size_t kOutputPoolCells = 2000;
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

// System-announce types that mean the set of ports moved and an enumeration is
// stale. PORT_SUBSCRIBED / PORT_UNSUBSCRIBED are deliberately NOT here: our own
// connect_from raises them, so counting them would make every refresh announce
// the next one and the engine would rebuild forever.
bool isPortSetChange (int type) noexcept
{
    switch (type)
    {
        case SND_SEQ_EVENT_CLIENT_START:
        case SND_SEQ_EVENT_CLIENT_EXIT:
        case SND_SEQ_EVENT_PORT_START:
        case SND_SEQ_EVENT_PORT_EXIT:
        case SND_SEQ_EVENT_PORT_CHANGE:
            return true;
        default:
            return false;
    }
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

    // Fired on the poll thread, outside seqMutex, when the announce port
    // reported a moved port set.
    DeviceChangeHandler onDeviceChange;

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
        // Hot-plug feed. Subscribing the input port to the System Announce port
        // routes client/port arrivals and departures into the same poll loop the
        // MIDI traffic uses. It is not in sourceIds (queryPorts skips client 0),
        // so disableAll never tears it down and it survives every refresh.
        if (inPort >= 0)
            snd_seq_connect_from (seq, inPort, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
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

    // Returns true when this batch carried an announce meaning the port set
    // moved. The caller fires the handler once the batch is drained and the
    // sequencer lock is released - it is consumer code, and holding seqMutex
    // across it would block enumerate() on the message thread.
    bool pumpEvents()
    {
        bool portSetChanged = false;
        const std::lock_guard<std::mutex> lock (seqMutex);
        snd_seq_event_t* ev = nullptr;
        while (snd_seq_event_input (seq, &ev) >= 0 && ev != nullptr)
        {
            // Announce traffic carries no MIDI, so it is taken before the decode
            // path rather than being sized and decoded as if it did.
            if (ev->source.client == SND_SEQ_CLIENT_SYSTEM)
            {
                portSetChanged = portSetChanged || isPortSetChange (ev->type);
                continue;
            }

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
        return portSetChanged;
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
            if (pumpEvents() && onDeviceChange)
                onDeviceChange();
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

void AlsaSeqMidiInput::setDeviceChangeHandler (DeviceChangeHandler h)
{
    impl->onDeviceChange = std::move (h);
}

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
    int               queue   = -1;
    bool              queueRunning = false;
    snd_midi_event_t* coder   = nullptr;
    struct Destination
    {
        int client = 0;
        int port = 0;
        std::uint16_t channels = 0;
        // Per producer stream, the latest queue time it has scheduled here.
        std::map<int, std::uint64_t> streamDeadlineNs {};
    };
    std::map<std::string, Destination> openDests;
    std::vector<pollfd> pollDescriptors;
    double latestQueuedTimeMs = 0.0;
    double queueClockOriginMs = 0.0;
    std::uint64_t queueClockOriginNs = 0;

    // As on the input side: the pump thread sends through `seq` while the
    // message thread can be opening or querying it. Taken by the public entry
    // points only - Impl::closeAll is also reached from the destructor, which
    // must not re-enter it.
    std::mutex seqMutex;

    bool ready() const noexcept
    {
        return seq != nullptr && coder != nullptr && outPort >= 0 && queue >= 0
            && ! pollDescriptors.empty();
    }

    Impl() { initialise(); }

    bool initialise()
    {
        if (ready()) return true;
        releaseClient();
        if (snd_seq_open (&seq, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK) < 0)
        { seq = nullptr; return false; }
        struct CloseOnFailure
        {
            Impl& owner;
            bool complete = false;
            ~CloseOnFailure() { if (! complete) owner.releaseClient(); }
        } cleanup { *this };
        if (snd_seq_set_client_name (seq, "Dusk Studio") < 0) return false;
        if (snd_seq_set_client_pool_output (seq, kOutputPoolCells) < 0) return false;
        outPort = snd_seq_create_simple_port (seq, "MIDI Out",
                      SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (outPort < 0 || snd_midi_event_new (kOutputCoderBufferBytes, &coder) != 0) return false;
        queue = snd_seq_alloc_named_queue (seq, "Dusk MIDI output");
        if (queue < 0) return false;
        const int count = snd_seq_poll_descriptors_count (seq, POLLOUT);
        if (count <= 0) return false;
        pollDescriptors.resize (static_cast<std::size_t> (count));
        if (snd_seq_poll_descriptors (seq, pollDescriptors.data(), static_cast<unsigned int> (count), POLLOUT) != count)
            return false;
        cleanup.complete = true;
        return true;
    }

    // A running queue's timer ticks whether or not anything is scheduled, so
    // the queue runs only while a destination is open. Every start resets the
    // queue clock, which is why the origin is taken here and nowhere else:
    // re-sampling it per block can invert closely spaced producer deadlines.
    bool startQueue()
    {
        if (queueRunning) return true;
        if (snd_seq_start_queue (seq, queue, nullptr) < 0 || snd_seq_drain_output (seq) < 0) return false;
        queueRunning = true;
        snd_seq_queue_status_t* status;
        snd_seq_queue_status_alloca (&status);
        const double beforeStatus = backendClockMs();
        const int statusResult = snd_seq_get_queue_status (seq, queue, status);
        const double afterStatus = backendClockMs();
        if (statusResult < 0) return false;
        const auto* time = snd_seq_queue_status_get_real_time (status);
        if (time->tv_nsec >= 1000000000) return false;
        queueClockOriginMs = (beforeStatus + afterStatus) * 0.5;
        queueClockOriginNs = static_cast<std::uint64_t> (time->tv_sec) * 1000000000 + time->tv_nsec;
        return true;
    }

    bool stopQueue()
    {
        if (! queueRunning) return true;
        queueRunning = false;
        return snd_seq_stop_queue (seq, queue, nullptr) >= 0 && snd_seq_drain_output (seq) >= 0;
    }

    ~Impl()
    {
        closeAll();
        releaseClient();
    }

    void releaseClient() noexcept
    {
        if (seq != nullptr)
        {
            if (queue >= 0) snd_seq_free_queue (seq, queue);
            snd_seq_close (seq);
        }
        if (coder != nullptr) snd_midi_event_free (coder);
        seq = nullptr;
        coder = nullptr;
        outPort = queue = -1;
        queueRunning = false;
        pollDescriptors.clear();
        latestQueuedTimeMs = 0.0;
        queueClockOriginMs = 0.0;
        queueClockOriginNs = 0;
        openDests.clear();
    }

    // Cancels everything this client has scheduled and silences whatever
    // already sounded. False when the cancel had to delete the queue.
    bool cancelPending()
    {
        const bool dropped = snd_seq_drop_output (seq) >= 0;
        if (! dropped && queue >= 0)
        {
            snd_seq_free_queue (seq, queue);
            queue = -1;
            queueRunning = false;
        }
        // Purging a scheduled note-off must not leave an already delivered note on.
        for (auto& entry : openDests)
        {
            auto& destination = entry.second;
            for (int channel = 0; channel < 16; ++channel)
                if ((destination.channels & (1u << channel)) != 0)
                    for (const unsigned int controller : { 120u, 123u })
                    {
                        snd_seq_event_t event;
                        snd_seq_ev_clear (&event);
                        snd_seq_ev_set_source (&event, static_cast<unsigned char> (outPort));
                        snd_seq_ev_set_dest (&event, destination.client, destination.port);
                        event.type = SND_SEQ_EVENT_CONTROLLER;
                        event.data.control.channel = static_cast<unsigned char> (channel);
                        event.data.control.param = controller;
                        snd_seq_ev_set_direct (&event);
                        snd_seq_event_output_direct (seq, &event);
                    }
            destination.channels = 0;
        }
        latestQueuedTimeMs = 0.0;
        return dropped;
    }

    void disconnectAll()
    {
        for (const auto& entry : openDests)
            snd_seq_disconnect_to (seq, outPort, entry.second.client, entry.second.port);
        openDests.clear();
    }

    void closeAll (bool retire = false)
    {
        if (seq == nullptr) return;
        if (! cancelPending()) retire = true;
        disconnectAll();
        if (! retire && ! stopQueue()) retire = true;
        if (retire) releaseClient();
    }

    // The client cannot be trusted any more: retire it. The seam sees its
    // routes gone and reopens them on a fresh client.
    bool fail (const char* operation, int error)
    {
        std::fprintf (stderr, "[Dusk Studio/MIDI] ALSA output %s failed: %s; output closed.\n",
                      operation, snd_strerror (error));
        closeAll (true);
        return false;
    }

    // A pool still full this long after its last deadline means the queue's
    // timer is not running (softirq starvation, say), not that the client is
    // broken. What is pending is already late, so it goes, and the routes stay.
    bool abandonPending (const char* operation)
    {
        std::fprintf (stderr, "[Dusk Studio/MIDI] ALSA output %s timed out; pending output dropped.\n",
                      operation);
        if (! cancelPending())
        {
            disconnectAll();
            releaseClient();
        }
        return false;
    }

    bool submit (snd_seq_event_t& event, double deadlineMs)
    {
        for (;;)
        {
            const int result = snd_seq_event_output_direct (seq, &event);
            if (result >= 0) return true;
            if (result != -EAGAIN && result != -EINTR) return fail ("send", result);
            if (result == -EINTR)
            {
                if (backendClockMs() >= deadlineMs) return abandonPending ("send");
                continue;
            }
            for (;;)
            {
                const double remaining = std::ceil (deadlineMs - backendClockMs());
                if (remaining <= 0.0) return abandonPending ("wait");
                const int timeout = static_cast<int> (std::min (remaining,
                    static_cast<double> (std::numeric_limits<int>::max())));
                const int waited = ::poll (pollDescriptors.data(), pollDescriptors.size(), timeout);
                if (waited < 0 && errno == EINTR) continue;
                if (waited < 0) return fail ("wait", -errno);
                if (waited == 0) return abandonPending ("wait");
                unsigned short revents = 0;
                const int status = snd_seq_poll_descriptors_revents (seq, pollDescriptors.data(),
                    static_cast<unsigned int> (pollDescriptors.size()), &revents);
                if (status < 0) return fail ("poll status", status);
                if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                    return fail ("poll status", -EIO);
                if ((revents & POLLOUT) != 0) break;
            }
        }
    }
};

AlsaSeqMidiOutput::AlsaSeqMidiOutput() : impl (std::make_unique<Impl>()) {}
AlsaSeqMidiOutput::~AlsaSeqMidiOutput() = default;

std::vector<BackendDeviceInfo> AlsaSeqMidiOutput::enumerate()
{
    std::vector<BackendDeviceInfo> out;
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    if (! impl->initialise()) return out;
    for (auto& p : queryPorts (impl->seq, /*wantRead*/ false))
        out.push_back ({ p.name, p.identifier });
    return out;
}

std::string AlsaSeqMidiOutput::migrateIdentifier (const std::string& legacy)
{
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    if (! impl->ready()) return {};
    return migrateLegacyAddress (impl->seq, /*wantRead*/ false, legacy);
}

bool AlsaSeqMidiOutput::open (const std::string& identifier)
{
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    if (! impl->initialise()) return false;
    if (impl->openDests.count (identifier)) return true;   // lazy open is idempotent
    for (auto& p : queryPorts (impl->seq, false))
    {
        if (p.identifier != identifier) continue;
        if (! impl->startQueue())
        {
            impl->releaseClient();
            return false;
        }
        const int rc = snd_seq_connect_to (impl->seq, impl->outPort, p.client, p.port);
        if (rc < 0 && rc != -EBUSY)   // -EBUSY: already subscribed = already connected
        {
            if (impl->openDests.empty() && ! impl->stopQueue()) impl->releaseClient();
            return false;
        }
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

bool AlsaSeqMidiOutput::send (const std::string& identifier, int stream, const dusk::MidiBuffer& events,
                              double baseTimeMs, double sampleRate)
{
    const std::lock_guard<std::mutex> lock (impl->seqMutex);
    if (! impl->ready() || ! std::isfinite (baseTimeMs)
        || ! std::isfinite (sampleRate) || sampleRate <= 0.0) return false;
    auto it = impl->openDests.find (identifier);
    if (it == impl->openDests.end()) return false;
    if (events.isEmpty()) return true;

    snd_seq_queue_status_t* status;
    snd_seq_queue_status_alloca (&status);
    const int statusResult = snd_seq_get_queue_status (impl->seq, impl->queue, status);
    const double afterStatus = backendClockMs();
    if (statusResult < 0) return impl->fail ("queue status", statusResult);
    const auto* queueTime = snd_seq_queue_status_get_real_time (status);
    if (queueTime->tv_nsec >= 1000000000) return impl->fail ("queue time", -ERANGE);
    const long double baseQueueNs = static_cast<long double> (impl->queueClockOriginNs)
        + (static_cast<long double> (baseTimeMs) - impl->queueClockOriginMs) * 1000000.0L;
    const auto queueNs = [baseQueueNs, sampleRate] (int offset, std::uint64_t& result)
    {
        constexpr std::uint64_t maximumNs = (static_cast<std::uint64_t> (
            std::numeric_limits<unsigned int>::max()) + 1) * 1000000000 - 1;
        const long double when = baseQueueNs + static_cast<long double> (offset) * 1000000000.0L / sampleRate;
        if (! std::isfinite (when) || when > static_cast<long double> (maximumNs)) return false;
        result = static_cast<std::uint64_t> (std::max (0.0L, std::round (when)));
        return true;
    };
    double latestBlockTimeMs = afterStatus;
    for (const auto meta : events)
    {
        const double dueMs = baseTimeMs + static_cast<double> (meta.samplePosition) * 1000.0 / sampleRate;
        std::uint64_t ns = 0;
        if (! std::isfinite (dueMs) || ! queueNs (meta.samplePosition, ns)) return false;
        latestBlockTimeMs = std::max (latestBlockTimeMs, dueMs);
    }
    constexpr double kOutputWaitGraceMs = 20.0;
    const double waitDeadlineMs = std::max (latestBlockTimeMs, impl->latestQueuedTimeMs) + kOutputWaitGraceMs;

    // The floor is the stream's deadline before this block, not a running
    // maximum: events within the block keep their own times, so a buffer
    // filled out of offset order (Clock, then MTC) still interleaves by time.
    auto& streamDeadlineNs = it->second.streamDeadlineNs[stream];
    const std::uint64_t floorNs = streamDeadlineNs;
    bool allOk = true;
    for (const auto meta : events)
    {
        const double dueMs = baseTimeMs + static_cast<double> (meta.samplePosition) * 1000.0 / sampleRate;
        std::uint64_t ns = 0;
        queueNs (meta.samplePosition, ns);
        ns = std::max (ns, floorNs);
        snd_seq_real_time_t time;
        time.tv_sec = static_cast<unsigned int> (ns / 1000000000);
        time.tv_nsec = static_cast<unsigned int> (ns % 1000000000);
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
            snd_seq_ev_set_dest (&ev, it->second.client, it->second.port);
            constexpr auto scheduleMask = static_cast<unsigned char> (
                ~(SND_SEQ_TIME_STAMP_MASK | SND_SEQ_TIME_MODE_MASK));
            ev.flags = static_cast<unsigned char> ((ev.flags & scheduleMask) | SND_SEQ_TIME_STAMP_REAL);
            ev.queue = static_cast<unsigned char> (impl->queue);
            ev.time.time = time;
            if (! impl->submit (ev, waitDeadlineMs)) return false;
            streamDeadlineNs = std::max (streamDeadlineNs, ns);
            impl->latestQueuedTimeMs = std::max (impl->latestQueuedTimeMs, dueMs);
            if (ev.type == SND_SEQ_EVENT_NOTEON && ev.data.note.velocity != 0)
                it->second.channels |= static_cast<std::uint16_t> (1u << ev.data.note.channel);
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
void AlsaSeqMidiInput::setDeviceChangeHandler (DeviceChangeHandler) {}
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
bool AlsaSeqMidiOutput::send (const std::string&, int, const dusk::MidiBuffer&, double, double) { return false; }
} // namespace duskstudio::midi

#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/midi/AlsaSeqMidi.h"

#include <alsa/asoundlib.h>
#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using duskstudio::midi::AlsaSeqMidiOutput;
using duskstudio::midi::backendClockMs;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kDestination = 42;
constexpr int kFirstPort = 7;
constexpr int kSecondPort = 8;
constexpr int kSourcePort = 3;
constexpr const char* kFirstId = "alsa-seq:Fixture:First";
constexpr const char* kSecondId = "alsa-seq:Fixture:Second";

struct Captured
{
    snd_seq_event_t event {};
    std::vector<std::uint8_t> bytes;
};

struct Client
{
    int id = 0;
    int queue = 0;
    bool nonblocking = false;
    bool closed = false;
    int starts = 0;
    int stops = 0;
    std::size_t poolCells = 500;
    snd_seq_real_time_t now { 7, 990000000 };
    // A START resets the real kernel queue clock. Each one here lands 100 s
    // further on, so a deadline mapped through a stale origin is plainly wrong.
    std::int64_t startNs = 7990000000LL;
    double startClockMs = 0.0;
    double statusClockMs = 0.0;
    std::int64_t statusOffsetNs = 0;
    std::vector<Captured> accepted;
    std::vector<Captured> attempts;
    std::vector<Captured> pending;
    std::vector<std::string> calls;
    std::deque<int> outputResults;
};

struct Fixture
{
    static Fixture* current;
    std::vector<std::unique_ptr<Client>> clients;
    std::map<const snd_seq_queue_status_t*, Client*> statuses;
    std::deque<int> pollResults;
    std::deque<int> pollErrors;
    std::vector<int> pollTimeouts;
    int allocationResult = 0;
    int startResult = 0;
    int drainResult = 0;
    int statusResult = 0;
    int dropResult = 0;
    int freeResult = 0;
    int descriptorResult = 1;
    int poolResult = 0;
    int descriptorStatusResult = 0;
    short descriptorEvents = POLLOUT;

    Fixture() { REQUIRE (current == nullptr); current = this; }
    ~Fixture() { current = nullptr; }
    Client& last() { return *clients.back(); }
};

Fixture* Fixture::current = nullptr;

Client& clientOf (snd_seq_t* handle)
{
    return *reinterpret_cast<Client*> (handle);
}

std::int64_t timestampNs (const snd_seq_event_t& event)
{
    return static_cast<std::int64_t> (event.time.time.tv_sec) * 1000000000
         + event.time.time.tv_nsec;
}

std::int64_t queueNowNs (const Client& client)
{
    return static_cast<std::int64_t> (client.now.tv_sec) * 1000000000 + client.now.tv_nsec;
}

dusk::MidiBuffer notes (int firstOffset, int secondOffset = -1, int channel = 0)
{
    dusk::MidiBuffer buffer;
    const std::uint8_t on[] { static_cast<std::uint8_t> (0x90 | channel), 60, 100 };
    REQUIRE (buffer.addEvent (on, 3, firstOffset));
    if (secondOffset >= 0)
    {
        const std::uint8_t off[] { static_cast<std::uint8_t> (0x80 | channel), 60, 0 };
        REQUIRE (buffer.addEvent (off, 3, secondOffset));
    }
    return buffer;
}

void expectScheduled (const Captured& captured, const Client& client, int destination = kFirstPort)
{
    CHECK (captured.event.queue == client.queue);
    CHECK ((captured.event.flags & SND_SEQ_TIME_STAMP_MASK) == SND_SEQ_TIME_STAMP_REAL);
    CHECK ((captured.event.flags & SND_SEQ_TIME_MODE_MASK) == SND_SEQ_TIME_MODE_ABS);
    CHECK (captured.event.source.port == kSourcePort);
    CHECK (captured.event.dest.client == kDestination);
    CHECK (captured.event.dest.port == destination);
    CHECK (captured.event.time.time.tv_nsec < 1000000000);
}

std::size_t callIndex (const Client& client, const std::string& call)
{
    const auto found = std::find (client.calls.begin(), client.calls.end(), call);
    return found == client.calls.end() ? client.calls.size()
                                      : static_cast<std::size_t> (found - client.calls.begin());
}
} // namespace

extern "C"
{
int __wrap_snd_seq_open (snd_seq_t** handle, const char*, int, int mode)
{
    auto& fixture = *Fixture::current;
    auto client = std::make_unique<Client>();
    client->id = 100 + static_cast<int> (fixture.clients.size());
    client->queue = static_cast<int> (fixture.clients.size());
    client->nonblocking = (mode & SND_SEQ_NONBLOCK) != 0;
    *handle = reinterpret_cast<snd_seq_t*> (client.get());
    fixture.clients.push_back (std::move (client));
    return 0;
}

int __wrap_snd_seq_close (snd_seq_t* handle)
{
    auto& client = clientOf (handle);
    client.calls.push_back ("close");
    client.pending.clear();
    client.closed = true;
    return 0;
}

int __wrap_snd_seq_set_client_name (snd_seq_t*, const char*) { return 0; }
int __wrap_snd_seq_client_id (snd_seq_t* handle) { return clientOf (handle).id; }
int __wrap_snd_seq_create_simple_port (snd_seq_t*, const char*, unsigned int, unsigned int)
{
    return kSourcePort;
}

int __wrap_snd_seq_nonblock (snd_seq_t* handle, int nonblock)
{
    clientOf (handle).nonblocking = nonblock != 0;
    return 0;
}

int __wrap_snd_seq_set_client_pool_output (snd_seq_t* handle, std::size_t size)
{
    if (Fixture::current->poolResult < 0) return Fixture::current->poolResult;
    clientOf (handle).poolCells = size;
    return 0;
}

int __wrap_snd_seq_query_next_client (snd_seq_t*, snd_seq_client_info_t* info)
{
    if (snd_seq_client_info_get_client (info) >= kDestination) return -ENOENT;
    snd_seq_client_info_set_client (info, kDestination);
    snd_seq_client_info_set_name (info, "Fixture");
    return 0;
}

int __wrap_snd_seq_query_next_port (snd_seq_t*, snd_seq_port_info_t* info)
{
    const int prior = snd_seq_port_info_get_port (info);
    const bool first = prior == 255 || prior < kFirstPort;
    if (! first && prior >= kSecondPort) return -ENOENT;
    const int port = first ? kFirstPort : kSecondPort;
    snd_seq_port_info_set_port (info, port);
    snd_seq_port_info_set_name (info, port == kFirstPort ? "First" : "Second");
    snd_seq_port_info_set_capability (info, SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
    return 0;
}

int __wrap_snd_seq_connect_to (snd_seq_t* handle, int, int, int port)
{
    clientOf (handle).calls.push_back ("connect:" + std::to_string (port));
    return 0;
}

int __wrap_snd_seq_disconnect_to (snd_seq_t* handle, int, int, int port)
{
    clientOf (handle).calls.push_back ("disconnect:" + std::to_string (port));
    return 0;
}

int __wrap_snd_seq_alloc_named_queue (snd_seq_t* handle, const char*)
{
    clientOf (handle).calls.push_back ("allocate");
    return Fixture::current->allocationResult < 0 ? Fixture::current->allocationResult : clientOf (handle).queue;
}

int __wrap_snd_seq_control_queue (snd_seq_t* handle, int, int type, int, snd_seq_event_t*)
{
    auto& client = clientOf (handle);
    if (type == SND_SEQ_EVENT_START)
    {
        client.calls.push_back ("start");
        ++client.starts;
        client.startClockMs = backendClockMs();
        client.startNs = 7990000000LL + (client.starts - 1) * 100000000000LL;
    }
    if (type == SND_SEQ_EVENT_STOP)
    {
        client.calls.push_back ("stop");
        ++client.stops;
    }
    return Fixture::current->startResult;
}

int __wrap_snd_seq_drain_output (snd_seq_t*) { return Fixture::current->drainResult; }

int __wrap_snd_seq_get_queue_status (snd_seq_t* handle, int, snd_seq_queue_status_t* status)
{
    auto& client = clientOf (handle);
    client.statusClockMs = backendClockMs();
    const auto now = client.startNs
        + static_cast<std::int64_t> (std::llround ((client.statusClockMs - client.startClockMs) * 1000000.0))
        + client.statusOffsetNs;
    client.now.tv_sec = static_cast<unsigned int> (now / 1000000000);
    client.now.tv_nsec = static_cast<unsigned int> (now % 1000000000);
    Fixture::current->statuses[status] = &client;
    return Fixture::current->statusResult;
}

const snd_seq_real_time_t* __wrap_snd_seq_queue_status_get_real_time (const snd_seq_queue_status_t* status)
{
    return &Fixture::current->statuses.at (status)->now;
}

int __wrap_snd_seq_free_queue (snd_seq_t* handle, int)
{
    auto& client = clientOf (handle);
    client.calls.push_back ("free");
    if (Fixture::current->freeResult >= 0) client.pending.clear();
    return Fixture::current->freeResult;
}

int __wrap_snd_seq_drop_output (snd_seq_t* handle)
{
    auto& client = clientOf (handle);
    client.calls.push_back ("drop");
    if (Fixture::current->dropResult >= 0) client.pending.clear();
    return Fixture::current->dropResult;
}

int __wrap_snd_seq_event_output_direct (snd_seq_t* handle, snd_seq_event_t* event)
{
    auto& client = clientOf (handle);
    Captured captured;
    captured.event = *event;
    if ((event->flags & SND_SEQ_EVENT_LENGTH_MASK) == SND_SEQ_EVENT_LENGTH_VARIABLE)
    {
        const auto* bytes = static_cast<const std::uint8_t*> (event->data.ext.ptr);
        captured.bytes.assign (bytes, bytes + event->data.ext.len);
        captured.event.data.ext.ptr = nullptr;
    }
    client.attempts.push_back (captured);
    if (event->queue != SND_SEQ_QUEUE_DIRECT && ! captured.bytes.empty()
        && 1 + (captured.bytes.size() + sizeof (snd_seq_event_t) - 1) / sizeof (snd_seq_event_t) > client.poolCells)
        return -ENOMEM;
    int result = 0;
    if (! client.outputResults.empty())
    {
        result = client.outputResults.front();
        client.outputResults.pop_front();
    }
    if (result < 0) return result;
    client.accepted.push_back (captured);
    if (event->queue != SND_SEQ_QUEUE_DIRECT) client.pending.push_back (captured);
    if (event->type == SND_SEQ_EVENT_CONTROLLER)
        client.calls.push_back ("cc:" + std::to_string (event->data.control.channel)
                               + ":" + std::to_string (event->data.control.param));
    return 0;
}

int __wrap_snd_seq_poll_descriptors_count (snd_seq_t*, short)
{
    return Fixture::current->descriptorResult;
}

int __wrap_snd_seq_poll_descriptors (snd_seq_t*, pollfd* descriptors, unsigned int space, short events)
{
    if (space == 0) return -EINVAL;
    descriptors[0] = { 1234, events, 0 };
    return 1;
}

int __wrap_snd_seq_poll_descriptors_revents (snd_seq_t*, pollfd* descriptors, unsigned int,
                                            unsigned short* events)
{
    *events = static_cast<unsigned short> (descriptors[0].revents);
    return Fixture::current->descriptorStatusResult;
}

int __wrap_poll (pollfd* descriptors, nfds_t count, int timeout)
{
    auto& fixture = *Fixture::current;
    fixture.pollTimeouts.push_back (timeout);
    int result = 1;
    if (! fixture.pollResults.empty())
    {
        result = fixture.pollResults.front();
        fixture.pollResults.pop_front();
    }
    if (result < 0)
    {
        errno = fixture.pollErrors.empty() ? EINTR : fixture.pollErrors.front();
        if (! fixture.pollErrors.empty()) fixture.pollErrors.pop_front();
    }
    if (result > 0)
        for (nfds_t i = 0; i < count; ++i) descriptors[i].revents = fixture.descriptorEvents;
    return result;
}
}

TEST_CASE ("ALSA output schedules producer deadlines and sample offsets", "[alsa-output-schedule]")
{
    for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        CAPTURE (rate);
        Fixture fixture;
        AlsaSeqMidiOutput output;
        REQUIRE (output.enumerate().size() == 2);
        const double beforeOpen = backendClockMs();
        REQUIRE (output.open (kFirstId));
        const double afterOpen = backendClockMs();
        auto& client = fixture.last();
        const double before = backendClockMs();
        const double base = before + 1000.0;
        REQUIRE (output.send (kFirstId, 0, notes (0, 1000), base, rate));
        const double after = backendClockMs();
        REQUIRE (client.accepted.size() == 2);
        for (const auto& event : client.accepted) expectScheduled (event, client);
        CHECK (client.queue == 0);
        CHECK (client.starts == 1);
        CHECK (client.accepted[0].event.time.time.tv_sec >= 8);
        CHECK_THAT (static_cast<double> (timestampNs (client.accepted[1].event)
                                        - timestampNs (client.accepted[0].event)),
                    WithinAbs (1000.0 * 1000000000.0 / rate, 2.0));
        CHECK_THAT (static_cast<double> (timestampNs (client.accepted[0].event) - queueNowNs (client)),
                    WithinAbs ((base - client.statusClockMs) * 1000000.0,
                               (afterOpen - beforeOpen + after - before) * 1000000.0 + 2.0));
    }
}

TEST_CASE ("ALSA output retains the producer clock after pump delay", "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    const double beforeOpen = backendClockMs();
    REQUIRE (output.open (kFirstId));
    const double afterOpen = backendClockMs();
    auto& client = fixture.last();
    const double before = backendClockMs();
    const double base = before - 200.0;
    REQUIRE (output.send (kFirstId, 0, notes (48000), base, 48000.0));
    const double after = backendClockMs();
    REQUIRE (client.accepted.size() == 1);
    expectScheduled (client.accepted.front(), client);
    const double expected = (base + 1000.0 - client.statusClockMs) * 1000000.0;
    CHECK_THAT (static_cast<double> (timestampNs (client.accepted.front().event) - queueNowNs (client)),
                WithinAbs (expected, (afterOpen - beforeOpen + after - before) * 1000000.0 + 2.0));
}

TEST_CASE ("ALSA output keeps one deadline across encoded SysEx fragments", "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    REQUIRE (output.open (kFirstId));
    std::size_t length = 70000;
    SECTION ("70 kB") {}
    SECTION ("200 kB") { length = 200000; }
    std::vector<std::uint8_t> sysex (length, 0x23);
    sysex.front() = 0xf0;
    sysex.back() = 0xf7;
    dusk::MidiBuffer buffer;
    REQUIRE (buffer.addEvent (sysex.data(), static_cast<int> (sysex.size()), 4800));
    REQUIRE (output.send (kFirstId, 0, buffer, backendClockMs() + 50.0, 48000.0));
    auto& client = fixture.last();
    CHECK (client.poolCells == 2000);
    REQUIRE (client.accepted.size() > 1);
    std::vector<std::uint8_t> combined;
    for (const auto& fragment : client.accepted)
    {
        expectScheduled (fragment, client);
        CHECK (fragment.event.type == SND_SEQ_EVENT_SYSEX);
        CHECK (fragment.bytes.size() <= 4096);
        CHECK (timestampNs (fragment.event) == timestampNs (client.accepted.front().event));
        combined.insert (combined.end(), fragment.bytes.begin(), fragment.bytes.end());
    }
    CHECK (combined == sysex);
    CHECK (client.accepted.front().bytes.front() == 0xf0);
    CHECK (client.accepted.back().bytes.back() == 0xf7);
}

TEST_CASE ("ALSA close purges pending events and sends panic before disconnect", "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    REQUIRE (output.open (kFirstId));
    REQUIRE (output.send (kFirstId, 0, notes (0, 4800, 2), backendClockMs() + 50.0, 48000.0));
    auto& client = fixture.last();
    client.calls.clear();
    output.closeAll();
    CHECK (client.pending.empty());
    CHECK_FALSE (output.isOpen (kFirstId));
    const auto drop = callIndex (client, "drop");
    const auto soundOff = callIndex (client, "cc:2:120");
    const auto notesOff = callIndex (client, "cc:2:123");
    const auto disconnect = callIndex (client, "disconnect:7");
    CHECK (drop < soundOff);
    CHECK (soundOff < disconnect);
    CHECK (drop < notesOff);
    CHECK (notesOff < disconnect);
    REQUIRE (output.open (kFirstId));
    REQUIRE (output.send (kFirstId, 0, notes (2400), backendClockMs() + 30.0, 48000.0));
    expectScheduled (client.accepted.back(), client);
    CHECK (fixture.clients.size() == 1);
}

TEST_CASE ("ALSA close leaves another output client's scheduled events intact", "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput first, second;
    REQUIRE (first.open (kFirstId));
    REQUIRE (second.open (kFirstId));
    REQUIRE (first.send (kFirstId, 0, notes (4800), backendClockMs() + 50.0, 48000.0));
    REQUIRE (second.send (kFirstId, 0, notes (4800), backendClockMs() + 50.0, 48000.0));
    auto& a = *fixture.clients[0];
    auto& b = *fixture.clients[1];
    REQUIRE (b.pending.size() == 1);
    first.closeAll();
    CHECK (a.pending.empty());
    CHECK (b.pending.size() == 1);
    CHECK (second.isOpen (kFirstId));
}

TEST_CASE ("ALSA output retries a full pool without moving or duplicating an event", "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    REQUIRE (output.open (kFirstId));
    auto& client = fixture.last();
    client.outputResults = { -EAGAIN, 0 };
    SECTION ("the descriptor becomes writable") {}
    SECTION ("a signal interrupts the descriptor wait")
    {
        fixture.pollResults = { -1, 1 };
        fixture.pollErrors = { EINTR };
    }
    REQUIRE (output.send (kFirstId, 0, notes (2400), backendClockMs() + 100.0, 48000.0));
    CHECK (client.nonblocking);
    REQUIRE (client.attempts.size() == 2);
    REQUIRE (client.accepted.size() == 1);
    CHECK (timestampNs (client.attempts.front().event) == timestampNs (client.attempts.back().event));
    expectScheduled (client.accepted.front(), client);
    CHECK (output.isOpen (kFirstId));
    CHECK_FALSE (fixture.pollTimeouts.empty());
}

TEST_CASE ("ALSA output retires a failed submission until explicit reopening", "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    REQUIRE (output.open (kFirstId));
    auto& client = fixture.last();
    client.outputResults = { 0, -EIO };
    SECTION ("a terminal write error follows a submitted note-on") {}
    SECTION ("waiting for the pool fails")
    {
        client.outputResults = { 0, -EAGAIN };
        fixture.pollResults = { -1 };
        fixture.pollErrors = { EIO };
    }
    SECTION ("the descriptor reports a device error")
    {
        client.outputResults = { 0, -EAGAIN };
        fixture.descriptorEvents = POLLERR;
    }
    SECTION ("descriptor status cannot be read")
    {
        client.outputResults = { 0, -EAGAIN };
        fixture.descriptorStatusResult = -EIO;
    }
    CHECK_FALSE (output.send (kFirstId, 0, notes (0, 4800, 4), backendClockMs() + 50.0, 48000.0));
    CHECK_FALSE (output.isOpen (kFirstId));
    CHECK (client.closed);
    CHECK (client.pending.empty());
    CHECK (callIndex (client, "drop") < callIndex (client, "cc:4:120"));
    CHECK (callIndex (client, "cc:4:123") < callIndex (client, "disconnect:7"));
    CHECK (callIndex (client, "disconnect:7") < callIndex (client, "close"));
    const auto attempts = client.attempts.size();
    CHECK_FALSE (output.send (kFirstId, 0, notes (0), backendClockMs(), 48000.0));
    CHECK (client.attempts.size() == attempts);
    REQUIRE (output.open (kFirstId));
    REQUIRE (fixture.clients.size() == 2);
    auto& replacement = fixture.last();
    CHECK_FALSE (replacement.closed);
    REQUIRE (output.send (kFirstId, 0, notes (2400), backendClockMs() + 50.0, 48000.0));
    REQUIRE (replacement.accepted.size() == 1);
    expectScheduled (replacement.accepted.front(), replacement);
}

TEST_CASE ("ALSA pool retry accounts for later events on another destination", "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    REQUIRE (output.open (kFirstId));
    REQUIRE (output.open (kSecondId));
    const double later = backendClockMs() + 500.0;
    REQUIRE (output.send (kFirstId, 0, notes (0), later, 48000.0));
    auto& client = fixture.last();
    client.outputResults = { -EAGAIN };
    fixture.pollResults = { 0 };
    const double before = backendClockMs();
    CHECK_FALSE (output.send (kSecondId, 0, notes (0), before, 48000.0));
    const double after = backendClockMs();
    REQUIRE (fixture.pollTimeouts.size() == 1);
    CHECK (fixture.pollTimeouts.front() >= static_cast<int> (std::floor (later - after)) + 20);
    CHECK (fixture.pollTimeouts.front() <= static_cast<int> (std::ceil (later - before)) + 20);
    CHECK_FALSE (client.closed);
    CHECK (client.pending.empty());
    CHECK (output.isOpen (kFirstId));
    CHECK (output.isOpen (kSecondId));
}

TEST_CASE ("ALSA output rejects incomplete queue initialisation", "[alsa-output-schedule]")
{
    Fixture fixture;
    bool clientUsable = false;
    SECTION ("queue allocation fails") { fixture.allocationResult = -ENOMEM; }
    SECTION ("no output descriptor exists") { fixture.descriptorResult = -EIO; }
    SECTION ("the output pool cannot be configured") { fixture.poolResult = -ENOMEM; }
    SECTION ("queue start fails") { fixture.startResult = -EIO; clientUsable = true; }
    SECTION ("queue start cannot drain") { fixture.drainResult = -EAGAIN; clientUsable = true; }
    SECTION ("the initial queue clock cannot be read") { fixture.statusResult = -EIO; clientUsable = true; }
    AlsaSeqMidiOutput output;
    CHECK (output.enumerate().empty() != clientUsable);
    CHECK_FALSE (output.open (kFirstId));
    CHECK_FALSE (output.send (kFirstId, 0, notes (0), backendClockMs(), 48000.0));
    for (const auto& client : fixture.clients) CHECK (client->closed);
}

TEST_CASE ("ALSA output retires a queue whose clock cannot be read", "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    REQUIRE (output.open (kFirstId));
    REQUIRE (output.send (kFirstId, 0, notes (0, 4800, 3), backendClockMs() + 50.0, 48000.0));
    auto& client = fixture.last();
    fixture.statusResult = -EIO;
    CHECK_FALSE (output.send (kFirstId, 0, notes (0), backendClockMs(), 48000.0));
    CHECK (client.closed);
    CHECK (client.pending.empty());
    CHECK_FALSE (output.isOpen (kFirstId));
    CHECK (callIndex (client, "drop") < callIndex (client, "cc:3:123"));
}

TEST_CASE ("ALSA cancellation retires the client if pending output cannot be dropped", "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    REQUIRE (output.open (kFirstId));
    REQUIRE (output.send (kFirstId, 0, notes (0, 4800, 1), backendClockMs() + 50.0, 48000.0));
    auto& client = fixture.last();
    fixture.dropResult = -EIO;
    SECTION ("deleting the queue cancels its events") {}
    SECTION ("queue deletion also fails") { fixture.freeResult = -EIO; }
    output.closeAll();
    CHECK (client.closed);
    CHECK (client.pending.empty());
    CHECK (callIndex (client, "drop") < callIndex (client, "free"));
    CHECK (callIndex (client, "free") < callIndex (client, "cc:1:120"));
    CHECK_FALSE (output.isOpen (kFirstId));
    fixture.dropResult = fixture.freeResult = 0;
    REQUIRE (output.open (kFirstId));
    CHECK (fixture.clients.size() == 2);
}

TEST_CASE ("ALSA producer deadlines stay stable across queue status sampling jitter", "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    REQUIRE (output.open (kFirstId));
    REQUIRE (output.open (kSecondId));
    auto& client = fixture.last();
    const double firstDue = backendClockMs() + 500.0;
    REQUIRE (output.send (kFirstId, 0, notes (0), firstDue, 48000.0));
    bool earlier = false;
    const char* route = kFirstId;
    int stream = 0;
    SECTION ("successive producer blocks retain their order") {}
    SECTION ("an intentionally earlier event on another route stays earlier")
    {
        earlier = true;
        route = kSecondId;
    }
    SECTION ("an earlier event from another producer on the same route stays earlier")
    {
        earlier = true;
        stream = 1;
    }
    client.statusOffsetNs = -20000000;
    REQUIRE (output.send (route, stream, notes (0), firstDue + (earlier ? -10.0 : 1.0), 48000.0));
    REQUIRE (client.accepted.size() == 2);
    CHECK_THAT (static_cast<double> (timestampNs (client.accepted[1].event)
                                    - timestampNs (client.accepted[0].event)),
                WithinAbs (earlier ? -10000000.0 : 1000000.0, 2.0));
}

TEST_CASE ("ALSA output keeps a producer's next block behind its last when the block clock jitters",
           "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    REQUIRE (output.open (kFirstId));
    auto& client = fixture.last();
    constexpr double rate = 48000.0;
    constexpr double blockMs = 512.0 * 1000.0 / rate;
    const double base = backendClockMs() + 100.0;
    double early = 0.27;
    SECTION ("the next callback read its clock 0.27 ms early") {}
    SECTION ("the next block's clock reading went backwards") { early = blockMs + 5.0; }

    dusk::MidiBuffer first;
    const std::uint8_t on[] { 0x90, 60, 100 };
    REQUIRE (first.addEvent (on, 3, 500));
    REQUIRE (output.send (kFirstId, 0, first, base, rate));

    dusk::MidiBuffer next;
    const std::uint8_t off[] { 0x80, 60, 0 };
    const std::uint8_t later[] { 0x90, 62, 100 };
    REQUIRE (next.addEvent (off, 3, 0));
    REQUIRE (next.addEvent (later, 3, 256));
    REQUIRE (output.send (kFirstId, 0, next, base + blockMs - early, rate));

    REQUIRE (client.accepted.size() == 3);
    const auto onNs = timestampNs (client.accepted[0].event);
    CHECK (timestampNs (client.accepted[1].event) == onNs);
    const double laterNominalNs = (blockMs - early + (256.0 - 500.0) * 1000.0 / rate) * 1000000.0;
    CHECK_THAT (static_cast<double> (timestampNs (client.accepted[2].event) - onNs),
                WithinAbs (std::max (0.0, laterNominalNs), 2.0));
}

TEST_CASE ("ALSA output times a block's events by offset whatever order they were added in",
           "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    REQUIRE (output.open (kFirstId));
    auto& client = fixture.last();
    const double base = backendClockMs() + 100.0;
    REQUIRE (output.send (kFirstId, 0, notes (0), base, 48000.0));

    // The sync emitters fill a block Clock first, then append MTC behind it.
    dusk::MidiBuffer sync;
    const std::uint8_t clock[] { 0xf8 };
    const std::uint8_t quarterFrame[] { 0xf1, 0x10 };
    REQUIRE (sync.addEvent (clock, 1, 400));
    REQUIRE (sync.addEvent (quarterFrame, 2, 100));
    REQUIRE (output.send (kFirstId, 0, sync, base + 512.0 * 1000.0 / 48000.0, 48000.0));

    REQUIRE (client.accepted.size() == 3);
    CHECK (client.accepted[1].event.type == SND_SEQ_EVENT_CLOCK);
    CHECK (client.accepted[2].event.type == SND_SEQ_EVENT_QFRAME);
    CHECK_THAT (static_cast<double> (timestampNs (client.accepted[2].event)
                                    - timestampNs (client.accepted[1].event)),
                WithinAbs (-300.0 * 1000000000.0 / 48000.0, 2.0));
}

TEST_CASE ("ALSA output keeps its client when the pool stays full past its grace", "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    REQUIRE (output.open (kFirstId));
    REQUIRE (output.open (kSecondId));
    auto& client = fixture.last();
    client.outputResults = { 0, -EAGAIN };
    fixture.pollResults = { 0 };
    CHECK_FALSE (output.send (kFirstId, 0, notes (0, 4800, 4), backendClockMs() + 50.0, 48000.0));
    CHECK_FALSE (client.closed);
    CHECK (client.pending.empty());
    CHECK (output.isOpen (kFirstId));
    CHECK (output.isOpen (kSecondId));
    CHECK (callIndex (client, "drop") < callIndex (client, "cc:4:120"));
    CHECK (callIndex (client, "cc:4:123") < client.calls.size());
    CHECK (callIndex (client, "disconnect:7") == client.calls.size());

    REQUIRE (output.send (kFirstId, 0, notes (2400), backendClockMs() + 50.0, 48000.0));
    REQUIRE (output.send (kSecondId, 0, notes (2400), backendClockMs() + 50.0, 48000.0));
    CHECK (fixture.clients.size() == 1);
    REQUIRE (client.pending.size() == 2);
    expectScheduled (client.pending[0], client);
    expectScheduled (client.pending[1], client, kSecondPort);
}

TEST_CASE ("ALSA output runs its queue only while an output is open", "[alsa-output-schedule]")
{
    Fixture fixture;
    AlsaSeqMidiOutput output;
    REQUIRE (output.enumerate().size() == 2);
    auto& client = fixture.last();
    CHECK (client.starts == 0);
    CHECK (callIndex (client, "allocate") < client.calls.size());

    REQUIRE (output.open (kFirstId));
    REQUIRE (output.open (kSecondId));
    CHECK (client.starts == 1);
    CHECK (client.stops == 0);

    client.calls.clear();
    output.closeAll();
    CHECK (client.stops == 1);
    CHECK (callIndex (client, "disconnect:8") < callIndex (client, "stop"));
    CHECK_FALSE (client.closed);

    const double before = backendClockMs();
    REQUIRE (output.open (kFirstId));
    CHECK (client.starts == 2);
    CHECK (fixture.clients.size() == 1);
    const double base = before + 50.0;
    REQUIRE (output.send (kFirstId, 0, notes (0), base, 48000.0));
    const double after = backendClockMs();
    REQUIRE (client.accepted.size() == 1);
    CHECK_THAT (static_cast<double> (timestampNs (client.accepted.front().event) - queueNowNs (client)),
                WithinAbs ((base - client.statusClockMs) * 1000000.0, (after - before) * 1000000.0 + 2.0));
}

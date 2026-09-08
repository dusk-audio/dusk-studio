#include "CoreMidiBackend.h"
#include "CoreMidiProtocol.h"

#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

// The device seam carries MIDI 1 bytes, including fragmented SysEx. Keep the
// byte-packet API here until the content model has a UMP representation.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace duskstudio::midi
{
namespace
{
class CallbackGate
{
public:
    // CoreMIDI serialises input on its receive thread. Admit at most one anyway
    // so a concurrent delivery cannot turn a source collector into MPSC.
    bool enter() noexcept
    {
        std::uint32_t idle = 0;
        return users.compare_exchange_strong (idle, 1, std::memory_order_acquire, std::memory_order_relaxed);
    }
    void leave() noexcept { users.fetch_sub (1, std::memory_order_release); }
    void open() noexcept { users.store (0, std::memory_order_release); }
    void close() noexcept { users.fetch_or (kClosed, std::memory_order_acq_rel); }
    void wait() const noexcept
    {
        while ((users.load (std::memory_order_acquire) & ~kClosed) != 0)
            std::this_thread::yield();
    }
private:
    static constexpr std::uint32_t kClosed = 1u << 31;
    std::atomic<std::uint32_t> users { kClosed };
};

struct AdmittedCallback
{
    CallbackGate& gate;
    ~AdmittedCallback() { gate.leave(); }
};

struct ChangeSubscription
{
    CallbackGate gate;
    IMidiInputBackend::DeviceChangeHandler handler;
};

bool changesEndpoints (const MIDINotification* notification)
{
    if (notification->messageID == kMIDIMsgObjectAdded || notification->messageID == kMIDIMsgObjectRemoved)
    {
        const auto type = reinterpret_cast<const MIDIObjectAddRemoveNotification*> (notification)->childType;
        return type == kMIDIObjectType_Source || type == kMIDIObjectType_Destination
            || type == kMIDIObjectType_ExternalSource || type == kMIDIObjectType_ExternalDestination;
    }
    if (notification->messageID != kMIDIMsgPropertyChanged) return false;
    const auto* change = reinterpret_cast<const MIDIObjectPropertyChangeNotification*> (notification);
    if (change->objectType == kMIDIObjectType_Other) return false;
    return CFEqual (change->propertyName, kMIDIPropertyName)
        || CFEqual (change->propertyName, kMIDIPropertyOffline)
        || CFEqual (change->propertyName, kMIDIPropertyUniqueID)
        || CFEqual (change->propertyName, kMIDIPropertyConnectionUniqueID);
}

struct ProcessClient
{
    MIDIClientRef client = 0;
    std::mutex mutex;
    std::vector<std::weak_ptr<ChangeSubscription>> subscribers;

    ProcessClient()
    {
        MIDIClientCreate (CFSTR ("Dusk Studio"), [] (const MIDINotification* n, void* context)
        {
            if (! changesEndpoints (n)) return;
            auto& self = *static_cast<ProcessClient*> (context);
            std::vector<std::shared_ptr<ChangeSubscription>> current;
            {
                const std::lock_guard<std::mutex> lock (self.mutex);
                for (auto it = self.subscribers.begin(); it != self.subscribers.end();)
                    if (auto subscriber = it->lock())
                    {
                        current.push_back (std::move (subscriber));
                        ++it;
                    }
                    else it = self.subscribers.erase (it);
            }
            for (const auto& subscriber : current)
                if (subscriber->gate.enter())
                {
                    const AdmittedCallback admitted { subscriber->gate };
                    if (subscriber->handler) subscriber->handler();
                }
        }, this, &client);
    }

    void subscribe (const std::shared_ptr<ChangeSubscription>& subscriber)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        subscribers.erase (std::remove_if (subscribers.begin(), subscribers.end(),
                                          [] (const auto& item) { return item.expired(); }), subscribers.end());
        subscribers.push_back (subscriber);
    }
};

ProcessClient& processClient()
{
    // Apple warns that disposing the last client can make later creation fail.
    // The framework releases this one process client at process termination.
    static auto* client = new ProcessClient;
    return *client;
}

std::string stringProperty (MIDIObjectRef object, CFStringRef property)
{
    CFStringRef value = nullptr;
    if (MIDIObjectGetStringProperty (object, property, &value) != noErr || value == nullptr) return {};
    const auto capacity = CFStringGetMaximumSizeForEncoding (CFStringGetLength (value), kCFStringEncodingUTF8) + 1;
    std::string result (static_cast<std::size_t> (capacity), '\0');
    if (! CFStringGetCString (value, result.data(), capacity, kCFStringEncodingUTF8)) result.clear();
    else result.resize (std::char_traits<char>::length (result.c_str()));
    CFRelease (value);
    return result;
}

BackendDeviceInfo objectInfo (MIDIObjectRef object)
{
    SInt32 id = 0;
    return { stringProperty (object, kMIDIPropertyName),
             MIDIObjectGetIntegerProperty (object, kMIDIPropertyUniqueID, &id) == noErr
                 ? std::to_string (id) : stringProperty (object, kMIDIPropertyUniqueID) };
}

bool startsWithDeviceName (const std::string& name, const std::string& prefix)
{
    if (prefix.empty()) return true;
    const auto value = CFStringCreateWithCString (nullptr, name.c_str(), kCFStringEncodingUTF8);
    const auto start = CFStringCreateWithCString (nullptr, prefix.c_str(), kCFStringEncodingUTF8);
    const bool result = value != nullptr && start != nullptr
        && CFStringFindWithOptions (value, start, CFRangeMake (0, CFStringGetLength (value)),
                                   kCFCompareAnchored | kCFCompareCaseInsensitive, nullptr);
    if (start != nullptr) CFRelease (start);
    if (value != nullptr) CFRelease (value);
    return result;
}

struct LegacyInfo
{
    BackendDeviceInfo deviceForm;
    BackendDeviceInfo endpointForm;
};

LegacyInfo legacyEndpointInfo (MIDIEndpointRef endpoint, bool external)
{
    auto info = objectInfo (endpoint);
    MIDIEntityRef entity = 0;
    MIDIEndpointGetEntity (endpoint, &entity);
    if (entity == 0) return { info, info };
    if (info.name.empty() && info.identifier.empty()) info = objectInfo (entity);
    MIDIDeviceRef device = 0;
    MIDIEntityGetDevice (entity, &device);
    if (device == 0) return { info, info };
    const auto deviceInfo = objectInfo (device);
    if (deviceInfo.name.empty() && deviceInfo.identifier.empty()) return { info, info };
    if (external && MIDIDeviceGetNumberOfEntities (device) < 2)
        return { coremidi::externalEndpointInfo (info, deviceInfo, coremidi::ExternalIdentifier::Device),
                 coremidi::externalEndpointInfo (info, deviceInfo, coremidi::ExternalIdentifier::Endpoint) };
    if (! startsWithDeviceName (info.name, deviceInfo.name))
    {
        info.name = deviceInfo.name + " " + info.name;
        while (! info.name.empty() && static_cast<unsigned char> (info.name.back()) <= ' ') info.name.pop_back();
        info.identifier = deviceInfo.identifier + " " + info.identifier;
    }
    return { info, info };
}

LegacyInfo connectedEndpointInfo (MIDIEndpointRef endpoint)
{
    LegacyInfo info;
    CFDataRef connections = nullptr;
    MIDIObjectGetDataProperty (endpoint, kMIDIPropertyConnectionUniqueID, &connections);
    if (connections != nullptr)
    {
        const auto* data = CFDataGetBytePtr (connections);
        const auto length = CFDataGetLength (connections);
        for (CFIndex offset = 0; offset + 4 <= length; offset += 4)
        {
            const std::uint32_t id = (std::uint32_t (data[offset]) << 24) | (std::uint32_t (data[offset + 1]) << 16)
                                  | (std::uint32_t (data[offset + 2]) << 8) | data[offset + 3];
            MIDIObjectRef object = 0;
            MIDIObjectType type = kMIDIObjectType_Other;
            if (MIDIObjectFindByUniqueID (static_cast<MIDIUniqueID> (id), &object, &type) != noErr) continue;
            LegacyInfo connected;
            if (type == kMIDIObjectType_ExternalSource || type == kMIDIObjectType_ExternalDestination)
                connected = legacyEndpointInfo (static_cast<MIDIEndpointRef> (object), true);
            else
            {
                const auto objectDetails = objectInfo (object);
                connected = { objectDetails, objectDetails };
            }
            coremidi::appendConnectedInfo (info.deviceForm, connected.deviceForm);
            coremidi::appendConnectedInfo (info.endpointForm, connected.endpointForm);
        }
        CFRelease (connections);
    }
    return info.deviceForm.name.empty() && info.deviceForm.identifier.empty() ? legacyEndpointInfo (endpoint, false) : info;
}

struct Endpoint
{
    MIDIEndpointRef endpoint;
    BackendDeviceInfo info;
    std::string deviceLegacyIdentifier;
    std::string endpointLegacyIdentifier;
};

std::vector<Endpoint> endpoints (bool input)
{
    std::vector<Endpoint> result;
    if (processClient().client == 0) return result;
    const auto count = input ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < count; ++i)
    {
        const auto endpoint = input ? MIDIGetSource (i) : MIDIGetDestination (i);
        SInt32 id = 0, offline = 0;
        if (endpoint == 0 || MIDIObjectGetIntegerProperty (endpoint, kMIDIPropertyUniqueID, &id) != noErr || id == 0) continue;
        if (MIDIObjectGetIntegerProperty (endpoint, kMIDIPropertyOffline, &offline) == noErr && offline != 0) continue;
        const auto legacy = connectedEndpointInfo (endpoint);
        result.push_back ({ endpoint, { legacy.deviceForm.name, coremidi::identifier (id) },
                            legacy.deviceForm.identifier, legacy.endpointForm.identifier });
    }
    return result;
}

std::vector<BackendDeviceInfo> enumerate (bool input)
{
    std::vector<BackendDeviceInfo> result;
    for (const auto& endpoint : endpoints (input)) result.push_back (endpoint.info);
    return result;
}

std::vector<coremidi::EndpointIdentity> identitySnapshot (bool input)
{
    std::vector<coremidi::EndpointIdentity> result;
    for (const auto& endpoint : endpoints (input))
        result.push_back ({ endpoint.info.identifier, endpoint.deviceLegacyIdentifier, endpoint.endpointLegacyIdentifier });
    return result;
}

std::string migrate (bool input, const std::string& legacy)
{
    return coremidi::migrateIdentifier (identitySnapshot (input), legacy);
}

coremidi::ClockAnchor clockAnchor (double millisecondsPerTick)
{
    const auto before = mach_absolute_time();
    const double milliseconds = backendClockMs();
    const auto after = mach_absolute_time();
    return { before + (after - before) / 2, milliseconds, millisecondsPerTick };
}

std::string legacyIdentifier (bool input, const std::string& identifier, const std::vector<BackendDeviceInfo>& available)
{
    return coremidi::legacyIdentifier (identitySnapshot (input), identifier, available);
}

double tickDuration()
{
    mach_timebase_info_data_t timebase {};
    if (mach_timebase_info (&timebase) != KERN_SUCCESS || timebase.denom == 0) return 0.0;
    return static_cast<double> (timebase.numer) / static_cast<double> (timebase.denom) / 1000000.0;
}

struct InputConnection
{
    CallbackGate gate;
    coremidi::PacketDecoder decoder;
    IMidiInputBackend::Receiver receiver;
    std::string identifier;
    double millisecondsPerTick = tickDuration();
};

struct InputPort
{
    MIDIPortRef port = 0;
    std::shared_ptr<InputConnection> connection;
};

class CoreMidiInput final : public IMidiInputBackend
{
public:
    ~CoreMidiInput() override { stop(); }
    std::vector<BackendDeviceInfo> enumerate() override { return midi::enumerate (true); }
    std::string migrateIdentifier (const std::string& legacy) override { return migrate (true, legacy); }
    void setReceiver (Receiver value) override { receiver = std::move (value); }
    void setDeviceChangeHandler (DeviceChangeHandler value) override { changeHandler = std::move (value); }

    bool enable (const std::string& identifier) override
    {
        if (ports.count (identifier) != 0) return true;
        for (const auto& endpoint : endpoints (true))
            if (endpoint.info.identifier == identifier && connect (identifier, endpoint.endpoint))
            {
                enabled.insert (identifier);
                return true;
            }
        return false;
    }

    void disableAll() override { stop(); enabled.clear(); }

    void start() override
    {
        if (running) return;
        for (const auto& endpoint : endpoints (true))
            if (enabled.count (endpoint.info.identifier) != 0 && ports.count (endpoint.info.identifier) == 0)
                connect (endpoint.info.identifier, endpoint.endpoint);
        changes = std::make_shared<ChangeSubscription>();
        changes->handler = changeHandler;
        processClient().subscribe (changes);
        changes->gate.open();
        for (const auto& entry : ports)
        {
            entry.second.connection->receiver = receiver;
            entry.second.connection->gate.open();
        }
        running = true;
    }

    void stop() override
    {
        if (changes != nullptr) changes->gate.close();
        for (const auto& entry : ports) entry.second.connection->gate.close();
        if (changes != nullptr) changes->gate.wait();
        for (const auto& entry : ports) entry.second.connection->gate.wait();
        for (const auto& entry : ports) MIDIPortDispose (entry.second.port);
        ports.clear();
        changes.reset();
        running = false;
    }

private:
    bool connect (const std::string& identifier, MIDIEndpointRef endpoint)
    {
        const auto state = std::make_shared<InputConnection>();
        if (! (state->millisecondsPerTick > 0.0)) return false;
        state->receiver = receiver;
        state->identifier = identifier;
        MIDIPortRef port = 0;
        if (MIDIInputPortCreateWithBlock (processClient().client, CFSTR ("Dusk MIDI Input"), &port,
            ^(const MIDIPacketList* packets, void*)
            {
                if (! state->gate.enter()) return;
                const AdmittedCallback admitted { state->gate };
                const auto anchor = clockAnchor (state->millisecondsPerTick);
                auto* packet = &packets->packet[0];
                for (UInt32 i = 0; i < packets->numPackets; ++i)
                {
                    state->decoder.push (packet->data, packet->length, anchor.toMilliseconds (packet->timeStamp),
                        [&] (const std::uint8_t* bytes, int count, double timeMs)
                        { if (state->receiver) state->receiver (state->identifier, bytes, count, timeMs); });
                    packet = MIDIPacketNext (packet);
                }
            }) != noErr) return false;
        if (MIDIPortConnectSource (port, endpoint, nullptr) != noErr)
        {
            MIDIPortDispose (port);
            return false;
        }
        ports.emplace (identifier, InputPort { port, state });
        if (running) state->gate.open();
        return true;
    }

    Receiver receiver;
    DeviceChangeHandler changeHandler;
    std::set<std::string> enabled;
    std::map<std::string, InputPort> ports;
    std::shared_ptr<ChangeSubscription> changes;
    bool running = false;
};

class CoreMidiOutput final : public IMidiOutputBackend
{
public:
    CoreMidiOutput()
    {
        if (MIDIOutputPortCreate (processClient().client, CFSTR ("Dusk MIDI Output"), &port) == noErr)
            worker = std::thread ([this] { run(); });
    }

    ~CoreMidiOutput() override
    {
        {
            const std::lock_guard<std::mutex> lock (mutex);
            running = false;
            pending.clear();
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
        if (port != 0) MIDIPortDispose (port);
    }

    std::vector<BackendDeviceInfo> enumerate() override { return midi::enumerate (false); }
    std::string migrateIdentifier (const std::string& legacy) override { return migrate (false, legacy); }

    bool open (const std::string& identifier) override
    {
        const std::lock_guard<std::mutex> lock (mutex);
        if (port == 0) return false;
        if (outputs.count (identifier) != 0) return true;
        for (const auto& endpoint : endpoints (false))
            if (endpoint.info.identifier == identifier)
            {
                outputs.emplace (identifier, endpoint.endpoint);
                return true;
            }
        return false;
    }

    void closeAll() override
    {
        const std::lock_guard<std::mutex> lock (mutex);
        outputs.clear();
        pending.clear();
        queuedBytes = 0;
        wake.notify_all();
    }

    bool isOpen (const std::string& identifier) const override
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return outputs.count (identifier) != 0;
    }

    bool send (const std::string& identifier, const dusk::MidiBuffer& events, double baseTimeMs, double sampleRate) override
    {
        const std::lock_guard<std::mutex> lock (mutex);
        const auto found = outputs.find (identifier);
        if (found == outputs.end() || ! std::isfinite (baseTimeMs)) return false;
        const double rate = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate : 48000.0;
        std::size_t bytes = 0;
        for (const auto event : events)
        {
            const double due = baseTimeMs + static_cast<double> (event.samplePosition) * 1000.0 / rate;
            if (! std::isfinite (due)) return false;
            bytes += sizeof (PendingMessage) + static_cast<std::size_t> (event.numBytes);
            if (bytes > kMaxQueuedBytes - queuedBytes) return false;
        }
        for (const auto event : events)
        {
            const double due = baseTimeMs + static_cast<double> (event.samplePosition) * 1000.0 / rate;
            pending.emplace (due, PendingMessage { found->second, { event.data, event.data + event.numBytes } });
        }
        queuedBytes += bytes;
        // Direct panic/control sends are due now and must reach CoreMIDI before
        // a caller immediately closes routes. Future events remain cancellable.
        const auto now = backendClockMs();
        bool sent = true;
        while (! pending.empty() && pending.begin()->first <= now)
            sent = sendFirstPending() && sent;
        wake.notify_all();
        return sent;
    }

private:
    struct PendingMessage
    {
        MIDIEndpointRef destination;
        std::vector<std::uint8_t> bytes;
    };

    void run()
    {
        std::unique_lock<std::mutex> lock (mutex);
        while (running)
        {
            if (pending.empty())
            {
                wake.wait (lock, [this] { return ! running || ! pending.empty(); });
                continue;
            }
            const double delayMs = pending.begin()->first - backendClockMs();
            if (delayMs > 0.0)
            {
                wake.wait_for (lock, std::chrono::duration<double, std::milli> (std::min (delayMs, 1000.0)));
                continue;
            }
            sendFirstPending();
        }
    }

    bool sendFirstPending()
    {
        auto item = pending.extract (pending.begin());
        const auto& message = item.mapped();
        queuedBytes -= sizeof (PendingMessage) + message.bytes.size();
        // Only due events enter CoreMIDI. Route closure can discard this
        // client's queue without flushing another client's destination.
        std::size_t offset = 0;
        while (offset < message.bytes.size())
        {
            auto* list = reinterpret_cast<MIDIPacketList*> (packetStorage.data());
            auto* packet = MIDIPacketListInit (list);
            const auto count = std::min<std::size_t> (message.bytes.size() - offset, 65500);
            if (MIDIPacketListAdd (list, packetStorage.size(), packet, 0, count, message.bytes.data() + offset) == nullptr
                || MIDISend (port, message.destination, list) != noErr) return false;
            offset += count;
        }
        return true;
    }

    static constexpr std::size_t kMaxQueuedBytes = 64 * dusk::kMidiRoutingBlockBytes;
    MIDIPortRef port = 0;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool running = true;
    std::map<std::string, MIDIEndpointRef> outputs;
    std::multimap<double, PendingMessage> pending;
    std::size_t queuedBytes = 0;
    alignas(MIDIPacketList) std::array<std::uint8_t, 65536> packetStorage {};
};

} // namespace

std::unique_ptr<IMidiInputBackend> makeCoreMidiInputBackend() { return std::make_unique<CoreMidiInput>(); }
std::unique_ptr<IMidiOutputBackend> makeCoreMidiOutputBackend() { return std::make_unique<CoreMidiOutput>(); }
std::string coreMidiLegacyInputIdentifier (const std::string& identifier, const std::vector<BackendDeviceInfo>& available)
{ return legacyIdentifier (true, identifier, available); }
std::string coreMidiLegacyOutputIdentifier (const std::string& identifier, const std::vector<BackendDeviceInfo>& available)
{ return legacyIdentifier (false, identifier, available); }
} // namespace duskstudio::midi

#pragma clang diagnostic pop

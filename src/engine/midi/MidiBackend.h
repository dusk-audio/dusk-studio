#pragma once

#include "../../foundation/MidiBuffer.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// JUCE-free backend interfaces behind the MIDI device seam. A platform backend
// (native ALSA sequencer on Linux; a JUCE-backed fallback on mac/win)
// implements these; the seam (MidiInputClient / MidiOutputBank) owns the
// index->identifier mapping and the per-input retiming collectors, so backends
// stay identifier-keyed and never see seam-order indices.
namespace duskstudio::midi
{
struct BackendDeviceInfo
{
    std::string name;
    std::string identifier;
};

// The one clock the MIDI path is timed against: backends stamp incoming events
// with it, and the seam drains its collectors against it. Both sides MUST read
// the same source - a backend stamping a different epoch than the drain
// compares to would retime every event by the offset between them.
inline double backendClockMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli> (steady_clock::now().time_since_epoch()).count();
}

class IMidiInputBackend
{
public:
    virtual ~IMidiInputBackend() = default;

    [[nodiscard]] virtual std::vector<BackendDeviceInfo> enumerate() = 0;

    // Fires on the backend's MIDI thread. deviceIdentifier matches enumerate();
    // timeMs is a hi-res ms timestamp in the same clock domain the seam's
    // MidiCollector drains against.
    using Receiver = std::function<void (const std::string& deviceIdentifier,
                                         const std::uint8_t* bytes, int numBytes,
                                         double timeMs)>;

    virtual void setReceiver (Receiver r) = 0;          // set once, before start

    // Fires on the backend's MIDI thread when the OS reports that the set of
    // MIDI ports moved. A bare signal, not a diff: the only useful response is
    // a full re-enumeration. One plug raises several, so the consumer coalesces.
    // The handler must not re-enter the backend - the rebuild that follows a
    // change calls stop(), which joins the thread the handler runs on.
    // Default no-op: only the ALSA sequencer has an announce port, so the JUCE
    // fallback leaves mac/win on the manual rescan path.
    using DeviceChangeHandler = std::function<void()>;
    virtual void setDeviceChangeHandler (DeviceChangeHandler) {}   // set once, before start

    // Best-effort remap of an identifier minted by a PREVIOUS backend, so a
    // session saved before the backend changed keeps its routing. Returns the
    // current identifier for that device, or "" when it cannot be mapped.
    // Message thread.
    [[nodiscard]] virtual std::string migrateIdentifier (const std::string& legacy) = 0;

    [[nodiscard]] virtual bool enable (const std::string& identifier) = 0;
    virtual void disableAll() = 0;
    virtual void start() = 0;                            // attach fence
    // Detach fence: joins the dispatch side before returning, so no receiver
    // callback is in flight once stop() returns (the contract the seam's
    // detach/rebuild/attach sequence relies on).
    virtual void stop() = 0;
};

class IMidiOutputBackend
{
public:
    virtual ~IMidiOutputBackend() = default;

    [[nodiscard]] virtual std::vector<BackendDeviceInfo> enumerate() = 0;

    // As IMidiInputBackend::migrateIdentifier, for output ports.
    [[nodiscard]] virtual std::string migrateIdentifier (const std::string& legacy) = 0;

    [[nodiscard]] virtual bool open (const std::string& identifier) = 0;   // lazy, message thread
    virtual void closeAll() = 0;
    [[nodiscard]] virtual bool isOpen (const std::string& identifier) const = 0;

    // Pump / message thread only; blocking is allowed. baseTimeMs + sampleRate
    // carry the sample-offset -> ms scheduling (mirrors JUCE's
    // sendBlockOfMessages semantics).
    [[nodiscard]] virtual bool send (const std::string& identifier,
                                     const dusk::MidiBuffer& events,
                                     double baseTimeMs, double sampleRate) = 0;
};
} // namespace duskstudio::midi

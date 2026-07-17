#pragma once

#include "../../foundation/MidiBuffer.h"

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

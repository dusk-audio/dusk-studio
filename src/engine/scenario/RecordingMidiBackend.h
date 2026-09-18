#pragma once

#include "../midi/MidiBackend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace duskstudio::scenario
{
// A MIDI output backend that delivers nowhere and records instead, so a
// scenario can assert on every message the output bank would have handed to the
// OS. MidiOutputBank calls send() from its pump thread (and from the message
// thread for a direct send), always under the bank's mutex, so there is one
// producer at a time: it appends into storage sized up front and publishes the
// new count, which the message thread reads back once the pump has drained.
class RecordingMidiBackend final : public midi::IMidiOutputBackend
{
public:
    // Every channel-voice and system-common message fits; a longer one is
    // recorded with its true numBytes and a truncated byte copy.
    static constexpr int kMaxMessageBytes = 8;

    struct Message
    {
        int    port         = -1;
        int    sampleOffset = 0;
        double timeMs       = 0.0;
        int    numBytes     = 0;
        std::array<std::uint8_t, kMaxMessageBytes> bytes {};
    };

    explicit RecordingMidiBackend (int numPorts = 1, int capacity = 4096)
    {
        for (int i = 0; i < std::max (1, numPorts); ++i)
        {
            const auto suffix = std::to_string (i + 1);
            ports.push_back ({ "Scenario Recorder " + suffix,
                               "dusk-studio:scenario-recorder-" + suffix });
        }
        openPorts.assign (ports.size(), false);
        recorded.resize ((std::size_t) std::max (1, capacity));
    }

    std::vector<midi::BackendDeviceInfo> enumerate() override { return ports; }

    std::string migrateIdentifier (const std::string&) override { return {}; }

    bool open (const std::string& identifier) override
    {
        const int port = indexOf (identifier);
        if (port < 0) return false;
        openPorts[(std::size_t) port] = true;
        return true;
    }

    void closeAll() override
    {
        std::fill (openPorts.begin(), openPorts.end(), false);
    }

    bool isOpen (const std::string& identifier) const override
    {
        const int port = indexOf (identifier);
        return port >= 0 && openPorts[(std::size_t) port];
    }

    bool send (const std::string& identifier, const dusk::MidiBuffer& events,
               double baseTimeMs, double sampleRate) override
    {
        const int port = indexOf (identifier);
        if (port < 0) return false;

        const double msPerSample = sampleRate > 0.0 ? 1000.0 / sampleRate : 0.0;
        auto w = writeIndex.load (std::memory_order_relaxed);

        for (const auto meta : events)
        {
            if (w >= recorded.size())
            {
                dropped.store (true, std::memory_order_relaxed);
                break;
            }

            auto& m = recorded[w];
            m.port         = port;
            m.sampleOffset = meta.samplePosition;
            m.timeMs       = baseTimeMs + meta.samplePosition * msPerSample;
            m.numBytes     = meta.numBytes;
            m.bytes.fill (0);
            std::memcpy (m.bytes.data(), meta.data,
                         (std::size_t) std::min (meta.numBytes, kMaxMessageBytes));
            ++w;
        }

        writeIndex.store (w, std::memory_order_release);
        return true;
    }

    // Message thread, after the bank's pump has drained.
    std::size_t count() const noexcept { return writeIndex.load (std::memory_order_acquire); }

    std::vector<Message> captured() const
    {
        const auto n = (std::ptrdiff_t) count();
        return { recorded.begin(), recorded.begin() + n };
    }

    // True once a send ran out of recorded storage, so a shortfall in count()
    // is not read as the engine having dropped the messages.
    bool hasDropped() const noexcept { return dropped.load (std::memory_order_relaxed); }

private:
    int indexOf (const std::string& identifier) const noexcept
    {
        for (int i = 0; i < (int) ports.size(); ++i)
            if (ports[(std::size_t) i].identifier == identifier)
                return i;
        return -1;
    }

    std::vector<midi::BackendDeviceInfo> ports;
    std::vector<bool> openPorts;

    std::vector<Message> recorded;
    std::atomic<std::size_t> writeIndex { 0 };
    std::atomic<bool> dropped { false };
};
} // namespace duskstudio::scenario

#pragma once

#include "MidiFit.h"
#include "../vst3/Vst3MidiNoteTracker.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace duskstudio::hosting
{
class StandardVst3Midi
{
public:
    // JUCE's VST3 wrapper stops reading after this many MIDI messages, controllers included.
    static constexpr std::size_t kMaxMessages = 2048;

    void prepare()
    {
        scratch.reserveBytes (dusk::kMidiRoutingBlockBytes);
    }

    // The legacy buffer must reserve kMidiRoutingBlockBytes before processing.
    // The packed seam uses at least as much storage per event as that buffer.
    template <typename Buffer>
    void rewrite (Buffer& input) noexcept
    {
        scratch.clear();
        deliver (input, [&] (const std::uint8_t* data, int bytes, int offset)
        { return scratch.addEvent (data, bytes, offset); });
        input.clear();
        for (const auto event : scratch)
            input.addEvent (event.data, event.numBytes, event.samplePosition);
    }

    void reset() noexcept
    {
        notes.reset();
        recovering = false;
    }

    template <typename Buffer, typename Emit>
    void deliver (const Buffer& input, Emit&& emit) noexcept
    {
        std::size_t sent = 0;
        const auto send = [&] (const std::uint8_t* data, int bytes, int offset)
        {
            if (sent == kMaxMessages || ! emit (data, bytes, offset)) return false;
            ++sent;
            return true;
        };
        const auto release = [&] (int channel, int offset)
        {
            return notes.releaseChannel (channel, [&] (int key)
            {
                const std::array<std::uint8_t, 3> off { (std::uint8_t) (0x80 | channel), (std::uint8_t) key, 0 };
                return send (off.data(), (int) off.size(), offset);
            });
        };

        if (recovering)
        {
            if (! midi::forEachTransportStopResetMessage ([&] (const std::uint8_t* data, int bytes)
                { return send (data, bytes, 0); })) return;
            for (int channel = 0; channel < 16; ++channel)
                if (! release (channel, 0)) return;
            recovering = false;
            return;
        }

        std::array<std::size_t, 16> releasable {};
        for (int channel = 0; channel < 16; ++channel)
            releasable[(std::size_t) channel] = notes.soundingOnChannel (channel);
        const auto cost = [releasable] (const std::uint8_t* data, int bytes) mutable -> std::size_t
        {
            if (bytes < 3) return 1;
            const auto status = data[0] & 0xF0;
            auto& count = releasable[data[0] & 0x0F];
            if (status == 0x90 && data[2] > 0) ++count;
            if (status == 0xB0 && (data[1] == 120 || data[1] == 123))
                return 1 + std::exchange (count, 0);
            return 1;
        };
        deliverWithinRoom (input, kMaxMessages, cost,
            [&] (const std::uint8_t* data, int bytes, int offset)
            {
                const auto status = data[0] & 0xF0;
                const int channel = data[0] & 0x0F;
                const bool onset = bytes >= 3 && status == 0x90 && data[2] > 0;
                if (onset && recovering) return;
                const bool accepted = send (data, bytes, offset);
                if (! accepted && endsNotesOrExpression (data, bytes)) recovering = true;
                if (bytes < 3) return;
                if (status == 0xB0 && (data[1] == 120 || data[1] == 123))
                {
                    if (! release (channel, offset)) recovering = true;
                }
                else if (accepted && onset) notes.noteOn (channel, data[1]);
                else if (accepted && (status == 0x80 || status == 0x90)) notes.noteOff (channel, data[1]);
            });
    }

private:
    dusk::MidiBuffer scratch;
    vst3::detail::MidiNoteTracker notes;
    bool recovering = false;
};
}

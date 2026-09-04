#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace duskstudio::vst3::detail
{
class MidiNoteTracker
{
public:
    void reset() noexcept { noteOnCounts.fill (0); }

    void noteOn (int channel, int key) noexcept
    {
        if (! valid (channel, key)) return;
        auto& count = noteOnCounts[index (channel, key)];
        if (count < std::numeric_limits<NoteCount>::max()) ++count;
    }

    void noteOff (int channel, int key) noexcept
    {
        if (! valid (channel, key)) return;
        auto& count = noteOnCounts[index (channel, key)];
        if (count > 0) --count;
    }

    bool isSounding (int channel, int key) const noexcept
    {
        return valid (channel, key) && noteOnCounts[index (channel, key)] > 0;
    }

    template <typename Emit>
    void releaseChannel (int channel, Emit&& emit) noexcept
    {
        if (channel < 0 || channel >= kChannels) return;
        for (int key = 0; key < kKeys; ++key)
        {
            auto& count = noteOnCounts[index (channel, key)];
            while (count > 0 && emit (key)) --count;
        }
    }

private:
    static constexpr int kChannels = 16;
    static constexpr int kKeys = 128;
    using NoteCount = std::uint16_t;

    static bool valid (int channel, int key) noexcept
    {
        return channel >= 0 && channel < kChannels && key >= 0 && key < kKeys;
    }

    static std::size_t index (int channel, int key) noexcept
    {
        return (std::size_t) (channel * kKeys + key);
    }

    std::array<NoteCount, (std::size_t) kChannels * kKeys> noteOnCounts {};
};
}

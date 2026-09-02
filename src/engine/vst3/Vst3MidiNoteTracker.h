#pragma once

#include <array>
#include <cstddef>

namespace duskstudio::vst3::detail
{
class MidiNoteTracker
{
public:
    void reset() noexcept { sounding.fill (false); }

    void noteOn (int channel, int key) noexcept
    {
        if (valid (channel, key)) sounding[index (channel, key)] = true;
    }

    void noteOff (int channel, int key) noexcept
    {
        if (valid (channel, key)) sounding[index (channel, key)] = false;
    }

    bool isSounding (int channel, int key) const noexcept
    {
        return valid (channel, key) && sounding[index (channel, key)];
    }

    template <typename Emit>
    void releaseChannel (int channel, Emit&& emit) noexcept
    {
        if (channel < 0 || channel >= kChannels) return;
        for (int key = 0; key < kKeys; ++key)
        {
            auto& active = sounding[index (channel, key)];
            if (active && emit (key)) active = false;
        }
    }

private:
    static constexpr int kChannels = 16;
    static constexpr int kKeys = 128;

    static bool valid (int channel, int key) noexcept
    {
        return channel >= 0 && channel < kChannels && key >= 0 && key < kKeys;
    }

    static std::size_t index (int channel, int key) noexcept
    {
        return (std::size_t) (channel * kKeys + key);
    }

    std::array<bool, (std::size_t) kChannels * kKeys> sounding {};
};
}

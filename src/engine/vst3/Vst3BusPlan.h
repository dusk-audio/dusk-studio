#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>

namespace duskstudio::vst3::detail
{
enum class AudioBusDirection { Input, Output };

// Keep the "activate every advertised bus" policy testable without loading a
// third-party module. Modular plugins may access auxiliary buses even when the
// mixer only exposes their selected main bus.
template <typename Activate>
void activateAllAudioBuses (int numInputs, int numOutputs, Activate&& activate)
{
    for (int bus = 0; bus < numInputs; ++bus)
        activate (AudioBusDirection::Input, bus);
    for (int bus = 0; bus < numOutputs; ++bus)
        activate (AudioBusDirection::Output, bus);
}

struct ScratchShape
{
    std::size_t inputChannels = 0;
    std::size_t outputChannels = 0;
    int widestBus = 2;
};

template <typename InputChannels, typename OutputChannels>
ScratchShape planScratch (int numInputs, int numOutputs,
                          InputChannels&& inputChannels,
                          OutputChannels&& outputChannels)
{
    ScratchShape result;
    for (int bus = 0; bus < numInputs; ++bus)
    {
        const int channels = std::max (0, inputChannels (bus));
        result.inputChannels += (std::size_t) channels;
        result.widestBus = std::max (result.widestBus, channels);
    }
    for (int bus = 0; bus < numOutputs; ++bus)
    {
        const int channels = std::max (0, outputChannels (bus));
        result.outputChannels += (std::size_t) channels;
        result.widestBus = std::max (result.widestBus, channels);
    }
    return result;
}

inline std::size_t scratchChannelOffset (std::size_t channelOrdinal,
                                         int maxFrames) noexcept
{
    return channelOrdinal * (std::size_t) std::max (1, maxFrames);
}
} // namespace duskstudio::vst3::detail

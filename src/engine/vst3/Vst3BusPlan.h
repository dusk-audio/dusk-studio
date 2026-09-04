#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace duskstudio::vst3::detail
{
enum class AudioBusDirection { Input, Output };

class AudioBusPlan
{
public:
    AudioBusPlan() = default;
    AudioBusPlan (int numInputs, int numOutputs)
        : inputs ((std::size_t) std::max (0, numInputs)),
          outputs ((std::size_t) std::max (0, numOutputs))
    {
    }

    void setBus (AudioBusDirection direction, int bus, int channels, bool active)
    {
        auto& buses = direction == AudioBusDirection::Input ? inputs : outputs;
        if (bus >= 0 && bus < (int) buses.size())
        {
            auto& entry = buses[(std::size_t) bus];
            entry.channels = std::max (0, channels);
            entry.active = active && entry.channels > 0;
        }
    }

    int busCount (AudioBusDirection direction) const noexcept
    {
        return (int) (direction == AudioBusDirection::Input ? inputs : outputs).size();
    }

    int channelCount (AudioBusDirection direction, int bus) const noexcept
    {
        const auto& buses = direction == AudioBusDirection::Input ? inputs : outputs;
        return bus >= 0 && bus < (int) buses.size()
                 ? buses[(std::size_t) bus].channels : 0;
    }

    bool isActive (AudioBusDirection direction, int bus) const noexcept
    {
        const auto& buses = direction == AudioBusDirection::Input ? inputs : outputs;
        return bus >= 0 && bus < (int) buses.size()
                 ? buses[(std::size_t) bus].active : false;
    }

    int processChannelCount (AudioBusDirection direction, int bus) const noexcept
    {
        return isActive (direction, bus) ? channelCount (direction, bus) : 0;
    }

private:
    struct Entry
    {
        int channels = 0;
        bool active = false;
    };

    std::vector<Entry> inputs;
    std::vector<Entry> outputs;
};

// Apply both activation and deactivation from the same shape used for layout
// metadata and process buffers. A plugin may reject deactivation while keeping
// a bus live; retain buffers for that bus so it never receives a null channel
// array.
template <typename Activate>
void applyAudioBusPlan (AudioBusPlan& plan, Activate&& activate)
{
    for (const auto direction : { AudioBusDirection::Input, AudioBusDirection::Output })
        for (int bus = 0; bus < plan.busCount (direction); ++bus)
        {
            const bool active = plan.isActive (direction, bus);
            if (! activate (direction, bus, active) && ! active)
                plan.setBus (direction, bus, plan.channelCount (direction, bus), true);
        }
}

template <typename InputChannels, typename OutputChannels>
bool matchesAudioBusShape (const AudioBusPlan& plan,
                           int numInputs, int numOutputs,
                           InputChannels&& inputChannels,
                           OutputChannels&& outputChannels)
{
    if (numInputs != plan.busCount (AudioBusDirection::Input)
        || numOutputs != plan.busCount (AudioBusDirection::Output))
        return false;

    for (int bus = 0; bus < numInputs; ++bus)
        if (inputChannels (bus) != plan.channelCount (AudioBusDirection::Input, bus))
            return false;
    for (int bus = 0; bus < numOutputs; ++bus)
        if (outputChannels (bus) != plan.channelCount (AudioBusDirection::Output, bus))
            return false;
    return true;
}

template <typename SetChannels>
void applyAudioBufferPlan (const AudioBusPlan& plan, SetChannels&& setChannels)
{
    for (const auto direction : { AudioBusDirection::Input, AudioBusDirection::Output })
        for (int bus = 0; bus < plan.busCount (direction); ++bus)
            setChannels (direction, bus, plan.processChannelCount (direction, bus));
}

struct ScratchShape
{
    std::size_t inputChannels = 0;
    std::size_t outputChannels = 0;
    int widestBus = 2;
};

inline ScratchShape planScratch (const AudioBusPlan& plan)
{
    ScratchShape result;
    for (const auto direction : { AudioBusDirection::Input, AudioBusDirection::Output })
    {
        for (int bus = 0; bus < plan.busCount (direction); ++bus)
        {
            const int channels = plan.processChannelCount (direction, bus);
            auto& total = direction == AudioBusDirection::Input
                            ? result.inputChannels : result.outputChannels;
            total += (std::size_t) channels;
            result.widestBus = std::max (result.widestBus, channels);
        }
    }
    return result;
}

inline std::size_t scratchChannelOffset (std::size_t channelOrdinal,
                                         int maxFrames) noexcept
{
    return channelOrdinal * (std::size_t) std::max (1, maxFrames);
}
} // namespace duskstudio::vst3::detail

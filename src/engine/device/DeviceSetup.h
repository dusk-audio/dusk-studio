#pragma once

#include "ChannelSet.h"

#include <string>

// The chosen device configuration, mirroring JUCE's AudioDeviceManager 
// AudioDeviceSetup: which output/input device, the rate and block size, and the
// active channel masks. The device manager reads this to open a device and hands
// it back so the selector UI and session persistence can round-trip it.
namespace duskstudio::device
{
struct DeviceSetup
{
    std::string outputDeviceName;
    std::string inputDeviceName;

    double sampleRate = 0.0;   // 0 = backend default
    int    bufferSize = 0;     // 0 = backend default

    ChannelSet inputChannels;
    ChannelSet outputChannels;

    // When true, the manager picks a sensible default channel set for the device
    // and ignores the mask above (the useDefault*Channels semantics JUCE used).
    bool useDefaultInputChannels  = true;
    bool useDefaultOutputChannels = true;
};
} // namespace duskstudio::device

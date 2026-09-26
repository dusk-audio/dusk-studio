#pragma once

#include "DeviceManager.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace duskstudio::device
{
// Picks the capture device a first launch should open, given the output device
// already chosen and the capture devices the backend offers.
//
// Preference order:
//   1. The entry whose name matches the output device's, exactly and then
//      case-insensitively. Most interfaces expose both directions under one
//      name, so this keeps a first launch on one piece of hardware rather than
//      pairing an interface's output with a webcam's microphone.
//   2. The first capture device the backend lists.
//
// Empty when the backend offers no capture devices, which the caller must read
// as "leave the input unset" rather than "open the empty-named default".
inline std::string chooseDefaultInputDevice (const std::string& outputDeviceName,
                                             const std::vector<std::string>& inputDeviceNames)
{
    if (inputDeviceNames.empty()) return {};

    if (! outputDeviceName.empty())
    {
        for (const auto& name : inputDeviceNames)
            if (name == outputDeviceName) return name;

        const auto fold = [] (std::string text)
        {
            std::transform (text.begin(), text.end(), text.begin(),
                            [] (unsigned char c) { return (char) std::tolower (c); });
            return text;
        };
        const auto wanted = fold (outputDeviceName);
        for (const auto& name : inputDeviceNames)
            if (fold (name) == wanted) return name;
    }

    return inputDeviceNames.front();
}

struct FirstLaunchInputResult
{
    std::string error;            // empty = the input opened alongside the output
    bool outputRestored = false;  // after an error: the setup it replaced is open again
};

// Reopens the device with the given capture device added to the output already
// open. The reopen closes that working output first, and a capture device on
// another card can refuse the output's rate or be busy; nothing is saved on a
// first launch, so without the restore every later launch would end the same
// way, with no device open at all.
inline FirstLaunchInputResult openWithFirstLaunchInput (DeviceManager& manager,
                                                        const std::string& inputDeviceName)
{
    const auto isWorking = [&manager]
    {
        auto* d = manager.getCurrentDevice();
        return d != nullptr && d->getCurrentSampleRate() > 0.0
            && d->getActiveOutputChannels().count() > 0;
    };

    const auto previous = manager.getSetup();
    auto setup = previous;
    setup.inputDeviceName = inputDeviceName;
    setup.useDefaultInputChannels = true;

    FirstLaunchInputResult result;
    result.error = manager.setSetup (setup, /*treatAsChosen*/ false);
    if (result.error.empty() && isWorking())
        return result;
    if (result.error.empty())
        result.error = "no working output after the reopen";
    result.outputRestored = manager.setSetup (previous, /*treatAsChosen*/ false).empty()
                            && isWorking();
    return result;
}
} // namespace duskstudio::device

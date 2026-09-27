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

// A device is open at a real rate with output channels active. The rate alone is
// not enough: a per-device ALSA name can resolve to no active outputs.
inline bool hasWorkingOutput (DeviceManager& manager)
{
    auto* d = manager.getCurrentDevice();
    return d != nullptr && d->getCurrentSampleRate() > 0.0
        && d->getActiveOutputChannels().count() > 0;
}

// Opens the current backend's default output with no input. An open that pairs
// an output with an input fails whole when the input does (an input on another
// card that refuses the output's rate, or on macOS one whose microphone prompt
// nobody answered, which CoreAudio fails with a timeout), so the output on its
// own is not a repeat of that failure. Not a chosen setup: nothing is saved.
inline bool openOutputAlone (DeviceManager& manager)
{
    auto* type = manager.getCurrentDeviceType();
    if (type == nullptr) return false;
    const auto outputs = type->getDeviceNames (/*wantInputNames*/ false);
    if (outputs.empty()) return false;
    const int index = type->getDefaultDeviceIndex (/*forInput*/ false);

    auto setup = manager.getSetup();
    setup.outputDeviceName = outputs[(index >= 0 && index < (int) outputs.size()) ? (size_t) index : 0];
    setup.inputDeviceName.clear();
    setup.inputChannels.clear();
    setup.useDefaultOutputChannels = true;
    setup.sampleRate = 0;
    setup.bufferSize = 0;
    return manager.setSetup (setup, /*treatAsChosen*/ false).empty() && hasWorkingOutput (manager);
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
//
// The input channels are named rather than left to the manager's default,
// because a manager initialised for outputs alone (a first launch waiting on
// microphone access) defaults to none.
inline FirstLaunchInputResult openWithFirstLaunchInput (DeviceManager& manager,
                                                        const std::string& inputDeviceName,
                                                        int numInputChannels)
{
    const auto previous = manager.getSetup();
    auto setup = previous;
    setup.inputDeviceName = inputDeviceName;
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    setup.inputChannels.setRange (0, numInputChannels, true);

    FirstLaunchInputResult result;
    result.error = manager.setSetup (setup, /*treatAsChosen*/ false);
    if (result.error.empty() && hasWorkingOutput (manager))
        return result;
    if (result.error.empty())
        result.error = "no working output after the reopen";
    result.outputRestored = manager.setSetup (previous, /*treatAsChosen*/ false).empty()
                            && hasWorkingOutput (manager);
    return result;
}
} // namespace duskstudio::device

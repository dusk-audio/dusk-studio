#pragma once

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
} // namespace duskstudio::device

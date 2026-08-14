#pragma once

#include "device/DeviceManager.h"

#include <string>

namespace duskstudio
{
// User-facing message after the startup audio-device open + busy-device
// fallback. Empty string = the saved setup opened fine (no alert needed).
// Pure + header-only so the branchy copy can be unit-tested without a real
// audio device (CI has none).
//
//   opened       : a device is open at a non-zero sample rate after fallback.
//   saved        : the persisted endpoint/backend intent (empty if unspecified).
//   actual       : the endpoint/backend open now (empty when unavailable).
inline std::string startupDeviceMessage (bool opened,
                                         const device::DeviceIdentity& saved,
                                         const device::DeviceIdentity& actual)
{
    const auto describe = [] (const device::DeviceIdentity& identity)
    {
        std::string description = identity.outputName.empty()
                                    ? "the default device"
                                    : ("\"" + identity.outputName + "\"");
        if (! identity.backendName.empty())
            description += " on the " + identity.backendName + " backend";
        return description;
    };

    if (opened)
    {
        const bool hasSavedIdentity = ! saved.outputName.empty() || ! saved.backendName.empty();
        const bool endpointChanged = ! saved.outputName.empty()
                                      && actual.outputName != saved.outputName;
        const bool backendChanged = ! saved.backendName.empty()
                                     && ! actual.backendName.empty()
                                     && actual.backendName != saved.backendName;
        // The saved setup opened (or there was nothing specific to compare) -
        // nothing to report. Endpoint names alone are insufficient on Windows:
        // shared and exclusive backends expose the same friendly device name.
        if (! hasSavedIdentity || (! endpointChanged && ! backendChanged))
            return {};

        // Fell back to a different endpoint or backend that works.
        return "Your saved audio setup for " + describe (saved)
             + " could not be opened - it "
               "may be in use by another application (another DAW, browser audio, or "
               "anything holding an exclusive driver) or no longer available.\n\n"
               "Audio has switched to " + describe (actual)
             + " so you can keep working. To use your original setup, free it in "
               "the other app, then reselect it in Audio Settings. Dusk Studio did not "
               "change your saved setup - it will be tried again next launch.";
    }

    // Nothing opened at all - the session is silent until a device frees up.
    std::string msg = "No audio device could be opened.\n\n";
    if (! saved.outputName.empty() || ! saved.backendName.empty())
        msg += "Your saved audio setup for " + describe (saved)
             + " appears to be in use by another application, and no other backend opened.";
    else
        msg += "No backend reported an available device.";
    // The load guards on every channel-strip insert refuse while the strips have
    // no sample rate, so name that here - otherwise the only symptom is a plugin
    // or soundfont that mysteriously won't load.
    msg += "\n\nThe playhead and meters will not move, recording is disabled, and "
           "plugins and soundfonts cannot be loaded until a device is open. Free the "
           "device in the other app, then open Audio Settings and select one.";
    return msg;
}
} // namespace duskstudio

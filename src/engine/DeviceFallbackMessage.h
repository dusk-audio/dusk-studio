#pragma once

#include <string>

namespace duskstudio
{
// User-facing message after the startup audio-device open + busy-device
// fallback. Empty string = the saved device opened fine (no alert needed).
// Pure + header-only so the branchy copy can be unit-tested without a real
// audio device (CI has none).
//
//   opened     : a device is open at a non-zero sample rate after the fallback.
//   savedName  : the device name the persisted setup asked for ("" if none).
//   actualName : the device actually open now ("" when opened == false).
inline std::string startupDeviceMessage (bool opened,
                                         const std::string& savedName,
                                         const std::string& actualName)
{
    if (opened)
    {
        // The saved device opened (or there was nothing specific to compare) -
        // nothing to report.
        if (savedName.empty() || actualName == savedName)
            return {};

        // Fell back to a different device that works.
        return "Your saved audio device \"" + savedName + "\" could not be opened - it "
               "may be in use by another application (PipeWire, JACK, browser audio, "
               "another DAW) or no longer available.\n\nAudio has switched to \"" + actualName
             + "\" so you can keep working. To use your original device, free it in "
               "the other app, then reselect it in Audio Settings. Dusk Studio did not "
               "change your saved device - it will be tried again next launch.";
    }

    // Nothing opened at all - the session is silent until a device frees up.
    std::string msg = "No audio device could be opened.\n\n";
    if (! savedName.empty())
        msg += "Your saved device \"" + savedName + "\" appears to be in use by another "
               "application (PipeWire, JACK, browser audio, or another DAW), and no other "
               "backend opened.";
    else
        msg += "No backend reported an available device.";
    msg += "\n\nThe playhead and meters will not move and recording is disabled until a "
           "device is available. Free the device in the other app, then open Audio "
           "Settings and select one.";
    return msg;
}
} // namespace duskstudio

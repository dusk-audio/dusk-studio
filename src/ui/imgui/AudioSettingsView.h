#pragma once

#include "DuskPanelWindow.h"

#include <functional>
#include <memory>
#include <string>

namespace duskstudio
{
class AudioEngine;
class Session;
namespace device { class DeviceManager; }
} // namespace duskstudio

namespace duskstudio::imgui
{
// What the settings panel cannot do from its own side of the framework boundary.
struct AudioSettingsHost
{
    int initialTab = 0;
    std::function<void (int)> tabChanged;

    // A device or ALSA-period failure, phrased for the user.
    std::function<void (std::string title, std::string message)> alert;

    // The live UI-scale preview. The shell owns the app-wide scale because changing it
    // re-lays out the whole window the panel is floating over, and it has to leave the
    // panel itself at the size it opened with or the control being dragged moves under
    // the pointer.
    std::function<void (float scale)> previewUiScale;

    // The shell closes this panel before opening another modal so the modal keeps focus.
    std::function<void()> openMidiBindings;
    std::function<void()> openSelfTest;
    std::function<void (bool moveExisting)> chooseSfzFolder;
    std::function<void()> choosePluginFolder;
    std::function<void()> scanPlugins;
};

std::unique_ptr<DuskPanelView> makeAudioSettingsView (device::DeviceManager& deviceManager,
                                                      AudioEngine& engine,
                                                      Session& session,
                                                      AudioSettingsHost host);
} // namespace duskstudio::imgui

#pragma once

#include "PanelControls.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::device { class DeviceManager; }

namespace duskstudio::imgui
{
// The audio settings panel's device block: backend, output device, input device,
// sample rate and buffer size, driving the dusk device manager directly.
//
// Output and input channels are opened wide (all the device exposes, clamped to the
// engine's limits) so the panel's main-output pair menu still finds every active
// pair - per-channel tick boxes are not reproduced.
class AudioDeviceSelector final
{
public:
    // `alert` carries a device-open failure to the shell, which is the only side that
    // can put a dialog in front of the user; `deviceChanged` fires after a successful
    // change so the owner can refresh what depends on the device.
    AudioDeviceSelector (device::DeviceManager& dm,
                         std::function<void (std::string title, std::string message)> alert,
                         std::function<void()> deviceChanged);
    ~AudioDeviceSelector();

    AudioDeviceSelector (const AudioDeviceSelector&) = delete;
    AudioDeviceSelector& operator= (const AudioDeviceSelector&) = delete;

    // The stack's height in design pixels; the panel sizes its audio block to this.
    static float preferredHeight() noexcept;

    void draw (DuskWidgets::Context& ctx, ImVec2 at, float width);

private:
    void rebuildFromManager();
    void applySetupChange (bool deviceChanged);
    void applyTypeChange();
    void deferred (std::function<void()> action);

    device::DeviceManager& deviceManager;
    std::function<void (std::string, std::string)> alert;
    std::function<void()> onDeviceChanged;

    // Handed to the deferred actions so one queued behind a panel that closes first
    // does not run against a destroyed selector.
    std::shared_ptr<int> life = std::make_shared<int> (0);

    ComboModel types, outputs, inputs, rates, buffers;
    std::vector<double> rateValues;
    std::vector<int> bufferValues;
    bool haveDevice = false;
    // The change listener fires while we are re-applying a setup; without this the
    // repopulation would be read as a user pick and applied again.
    bool updating = false;
};
} // namespace duskstudio::imgui

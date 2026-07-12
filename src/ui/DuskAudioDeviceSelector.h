#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "DuskComboBox.h"

namespace duskstudio
{
// In-window replacement for juce::AudioDeviceSelectorComponent's core
// controls (backend, output/input device, sample rate, buffer size). The
// stock JUCE selector pops its own native AlertWindow ("Error when trying to
// open audio device!") on failure, which can't be intercepted from app code
// and breaks the in-window-modal convention used everywhere else. This drives
// the same juce::AudioDeviceManager directly and surfaces open errors through
// showDuskAlert against the top-level window instead.
//
// Output + input channels are opened wide (all the device exposes, clamped to
// the engine's limits) so AudioSettingsPanel's main-output pair menu still
// finds every active pair - channel check-boxes aren't reproduced.
class DuskAudioDeviceSelector final : public juce::Component,
                                       public juce::ChangeListener
{
public:
    explicit DuskAudioDeviceSelector (juce::AudioDeviceManager& dm);
    ~DuskAudioDeviceSelector() override;

    void resized() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // Stacked label+combo rows; AudioSettingsPanel sizes the block to this.
    int getPreferredHeight() const noexcept;

    // Fired after a successful device change so the host can refresh dependent
    // UI (e.g. the main-output pair menu).
    std::function<void()> onDeviceChanged;

private:
    void rebuildFromManager();          // repopulate every combo from current state
    void applySetupChange (bool deviceChanged);  // build setup, apply, alert on error

    juce::AudioDeviceManager& deviceManager;
    bool updating = false;              // guard so repopulation doesn't re-apply

    juce::Label    typeLabel   { {}, "Audio backend" };
    DuskComboBox   typeCombo;
    juce::Label    outputLabel { {}, "Output device" };
    DuskComboBox   outputCombo;
    juce::Label    inputLabel  { {}, "Input device" };
    DuskComboBox   inputCombo;
    juce::Label    rateLabel   { {}, "Sample rate" };
    DuskComboBox   rateCombo;
    juce::Label    bufferLabel { {}, "Buffer size" };
    DuskComboBox   bufferCombo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DuskAudioDeviceSelector)
};
} // namespace duskstudio

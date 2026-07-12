#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "DuskComboBox.h"
#include "../session/Session.h"

namespace duskstudio
{
// In-window panel for configuring a HardwareInsertParams. Mirrors Logic
// Pro's I/O insert dialog: Output / Output Volume / Input / Input Volume
// / Latency Detection (Ping) / Latency Offset / Dry-Wet / Stereo|Mid-Side.
//
// Hosted inside an EmbeddedModal owned by the calling channel-strip /
// aux-lane component. The panel mutates `params` directly via the same
// AtomicSnapshot::publish + atomic-store pattern the rest of Dusk Studio uses
// for cross-thread parameter updates, so changes take effect on the
// audio thread one block after the user moves a control.
//
// The class already owns pingButton + timerCallback (the 10 Hz poller
// that reads HardwareInsertParams::pingResult and writes the measured
// lag back into latencySlider on success). Automatic / periodic re-pinging
// (e.g. on session load, or when the device sample rate changes) is not
// implemented. The manual Ping button drives
// the existing handshake today.
class HardwareInsertEditor final : public juce::Component,
                                       private juce::Timer
{
public:
    HardwareInsertEditor (HardwareInsertParams& params,
                          juce::AudioDeviceManager& deviceManager,
                          std::function<void()> onDone,
                          bool embedded = false);
    ~HardwareInsertEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kPanelW = 480;
    static constexpr int kPanelH = 460;

private:
    void populateDropdowns();
    HardwareInsertRouting currentRouting() const;
    void publishRoutingFromUi();
    void timerCallback() override;

    HardwareInsertParams& params;
    juce::AudioDeviceManager& deviceManager;
    std::function<void()> onDoneCallback;
    bool isEmbedded = false;

    juce::Label headerLabel;

    juce::Label   outVolLabel;
    juce::Slider  outVolSlider;
    juce::Label   outChLabel;
    DuskComboBox outChCombo;

    juce::Label   inChLabel;
    DuskComboBox inChCombo;
    juce::Label   inVolLabel;
    juce::Slider  inVolSlider;

    juce::Label      latencyLabel;
    juce::TextButton pingButton  { "Ping" };
    // Inline status next to the Ping button. Reads "Measuring..." while
    // in-flight, "Detected: N sam (X ms)" on success (green), or
    // "Ping failed - check level / cables" on failure (red). Replaces
    // the AlertWindow popup so the user keeps their eyes on the editor.
    juce::Label      pingStatusLabel;
    juce::Label      latencySamplesLabel;
    juce::Slider     latencySlider;

    juce::Label  dryWetLabel;
    juce::Slider dryWetSlider;

    juce::Label       formatLabel;
    juce::ToggleButton formatStereoButton  { "Stereo" };
    juce::ToggleButton formatMidSideButton { "Mid/Side" };

    juce::TextButton cancelButton { "Cancel" };
    juce::TextButton doneButton   { "Done" };
};
} // namespace duskstudio

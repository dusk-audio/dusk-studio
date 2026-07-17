#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <vector>
#include "EmbeddedModal.h"
#include "DuskComboBox.h"
#include "DuskAudioDeviceSelector.h"

namespace duskstudio
{
class AudioEngine;
class Session;

// Wraps juce::AudioDeviceSelectorComponent + adds a Periods combo
// (the only config knob JUCE's stock selector doesn't expose, but
// materially affects USB audio on Linux - fractional periods cause
// jitter; certain kernel+device combos distort at specific counts).
// Also hosts a "Run Self-Test" button (SelfTestPanel).
class AudioSettingsPanel final : public juce::Component,
                                  public juce::ChangeListener
{
public:
    AudioSettingsPanel (device::DeviceManager& dm,
                         AudioEngine& engine,
                         Session& session);
    ~AudioSettingsPanel() override;

    void paint   (juce::Graphics&) override;
    void resized() override;
    // Engine broadcasts on MIDI-bank rebuild (hot-plug, manual rescan).
    // Refreshes the sync-source combo so new inputs appear without
    // reopening. The device manager relays its own change (audio device
    // switch / active-output change) through this same handler.
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    device::DeviceManager& deviceManager;
    AudioEngine& engine;
    Session& session;
    std::unique_ptr<DuskAudioDeviceSelector> selector;

    // Physical output pair for the main mix. Defaults to outputs 1-2; pick
    // another pair when the device has more outputs enabled.
    juce::Label    mainOutputLabel { {}, "Main output" };
    DuskComboBox mainOutputCombo;
    void populateMainOutputCombo();
    void applyMainOutputChange();

#if defined(__linux__)
    juce::Label    periodsLabel  { {}, "Periods (ALSA)" };
    DuskComboBox periodsCombo;
#endif
    juce::TextButton selfTestButton { "Run Self-Test..." };

    // Re-enumerates all backends so hot-plugged devices appear without
    // restarting Dusk Studio. Most relevant on Linux USB (no OS notify).
    juce::TextButton rescanButton  { "Rescan devices" };

    // 2× / 4× engage internal oversampling on master + aux bus comps
    // and the master tape sat. Per-channel comp + EQ stay native.
    juce::Label    oversamplingLabel { {}, "Effect Oversampling" };
    DuskComboBox oversamplingCombo;

    // Per-machine (AppConfig): fan the 24 strips' DSP across worker threads.
    // Off / Auto (cores-2) / a pinned count. Live-applied via the same
    // detach/reattach re-prepare as oversampling.
    juce::Label    multicoreLabel { {}, "Multicore DSP" };
    DuskComboBox multicoreCombo;

    // Engine resolves selection to a real input index on every
    // hot-plug rebuild and feeds clock bytes to MidiSyncReceiver.
    juce::Label    syncSourceLabel { {}, "MIDI Sync Source" };
    DuskComboBox syncSourceCombo;
    // FA/FB Start + FC Stop drive Transport state, not just tempo.
    juce::ToggleButton syncChaseTransportToggle { "Chase transport (Start/Stop)" };

    juce::Label    syncOutputLabel { {}, "MIDI Sync Output" };
    DuskComboBox syncOutputCombo;
    juce::ToggleButton syncEmitClockToggle { "Emit clock (Dusk Studio as master)" };

    // MTC slave - when on AND receiver rolling, engine locks playhead
    // to master frame count on rising edge + freewheels with re-locate.
    juce::ToggleButton mtcChaseToggle { "Chase transport from MTC" };

    // MTC master - emit QF + full-frame sysex on the existing Sync Output.
    juce::ToggleButton mtcEmitToggle { "Emit MTC (Dusk Studio as master)" };
    juce::Label        mtcEmitFrameRateLabel { {}, "MTC frame rate" };
    DuskComboBox     mtcEmitFrameRateCombo;

    // MCU input (faders/buttons/encoders) + output (motor faders/LEDs/
    // LCD/timecode/meters). Device identifier persists in session.mcu;
    // ephemeral state (bank, selected ch, assign mode) is atomics.
    juce::Label    mcuInputLabel  { {}, "MCU Control Surface Input" };
    DuskComboBox mcuInputCombo;
    juce::Label    mcuOutputLabel { {}, "MCU Control Surface Output" };
    DuskComboBox mcuOutputCombo;
    void populateMcuInputCombo();
    void populateMcuOutputCombo();
    void applyMcuInputChange();
    void applyMcuOutputChange();

    // Per-target right-click is the primary add path; this modal is
    // for audit / cleanup of everything in one place.
    juce::TextButton midiBindingsButton { "MIDI Bindings..." };
    EmbeddedModal    midiBindingsModal;
    EmbeddedModal    selfTestModal;

    juce::Label  uiScaleLabel  { {}, "UI scale" };
    juce::Slider uiScaleSlider;
    juce::Label  uiScaleHint;
    bool         uiScaleDragging = false;

    // Per-machine (AppConfig), independent of session. Stderr log shows
    // added / total counts after MainComponent wires the engine.
    juce::ToggleButton scanOnStartupToggle { "Scan plugins on startup" };

    juce::Label audioSectionLabel         { {}, "Audio" };
    juce::Label controlSurfaceSectionLabel{ {}, "Control Surface" };
    juce::Label midiBindingsSectionLabel  { {}, "MIDI Bindings" };
    juce::Label midiSyncSectionLabel      { {}, "MIDI Sync" };
    juce::Label generalSectionLabel       { {}, "General" };
    juce::Label advancedSectionLabel      { {}, "Advanced" };
    juce::ToggleButton tapeStripExpandedToggle { "Expand tape strip by default" };
    juce::ToggleButton followPlayheadToggle    { "Follow playhead by default" };
    juce::ToggleButton softTakeoverToggle      { "MIDI soft takeover (pickup)" };
    juce::Label        stopBehaviorLabel       { {}, "Playhead on Stop:" };
    DuskComboBox       stopBehaviorCombo;
    juce::Label        autosaveLabel           { {}, "Autosave every:" };
    DuskComboBox       autosaveCombo;

    // Captured during resized() + drawn by paint() as thin horizontal
    // rules between section groups.
    std::vector<int> separatorYs;

#if defined(__linux__)
    void applyPeriodsChange();
#endif
    void applyOversamplingChange();
    void applyMulticoreChange();
    void applyUiScaleChange();
    void applyRescan();
    void openSelfTest();
    void populateSyncSourceCombo();
    void applySyncSourceChange();
    void populateSyncOutputCombo();
    void applySyncOutputChange();
};
} // namespace duskstudio

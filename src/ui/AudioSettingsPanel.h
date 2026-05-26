#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <vector>
#include "EmbeddedModal.h"
#include "DuskComboBox.h"

// The ALSA "periods per buffer" knob is owned by Dusk Studio's custom ALSA
// backend now (see AlsaAudioIODevice::set/getRequestedPeriods); the UI
// reads/writes that backend's atomic and triggers a re-open so the new
// value takes effect on the next snd_pcm_hw_params_set_periods_near call.
// AudioSettingsPanel.cpp pulls in the header where it actually uses the
// setter; the .h here keeps the dependency narrow.

namespace duskstudio
{
class AudioEngine;
class Session;

// Wraps juce::AudioDeviceSelectorComponent and adds a Periods combo at the
// bottom. Periods is the only audio-config knob JUCE's stock selector does
// not expose, but it materially affects USB audio quality on Linux -
// fractional periods cause jitter, and certain plug+kernel combos produce
// distortion or xruns at specific counts. Exposing the knob lets the user
// tune for their hardware without rebuilding.
//
// Also hosts a "Run Self-Test" button that opens the SelfTestPanel - a
// headless test of the audio engine pipeline plus a backend cycle.
class AudioSettingsPanel final : public juce::Component,
                                  public juce::ChangeListener
{
public:
    AudioSettingsPanel (juce::AudioDeviceManager& dm,
                         AudioEngine& engine,
                         Session& session);
    ~AudioSettingsPanel() override;

    void paint   (juce::Graphics&) override;
    void resized() override;
    // ChangeListener: the engine broadcasts on MIDI-bank rebuild (hot-
    // plug, manual rescan). Refreshes the sync-source combo so newly
    // appearing inputs become available without reopening the panel.
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    juce::AudioDeviceManager& deviceManager;
    AudioEngine& engine;
    Session& session;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;

#if defined(__linux__)
    juce::Label    periodsLabel  { {}, "Periods (ALSA)" };
    DuskComboBox periodsCombo;
#endif
    juce::TextButton selfTestButton { "Run Self-Test..." };

    // Rescan button: triggers re-enumeration of all registered audio
    // backends so freshly plugged-in / removed devices appear in the
    // dropdowns without restarting Dusk Studio. Most relevant for USB hot-plug
    // on Linux where there's no OS-level notification path; also fine on
    // mac/Windows since AudioIODeviceType::scanForDevices is universal.
    juce::TextButton rescanButton  { "Rescan devices" };

    // Global effect oversampling - single source of truth for "1× / 2× / 4×
    // across all effects". Default 1× (lowest CPU). 2× / 4× engage internal
    // oversampling on master + aux bus comps and the master tape sat
    // oversampler. Per-channel comp + EQ stay at native rate regardless.
    juce::Label    oversamplingLabel { {}, "Effect Oversampling" };
    DuskComboBox oversamplingCombo;

    // MIDI Clock sync source. Lists every registered MIDI input plus
    // "(none)" as the first entry. The engine resolves the selection
    // to a real input index on every hot-plug rebuild and feeds clock
    // bytes from that input to its MidiSyncReceiver. v1 follows tempo
    // only - the chase + Start/Stop chase land in a later phase.
    juce::Label    syncSourceLabel { {}, "MIDI Sync Source" };
    DuskComboBox syncSourceCombo;
    // When checked, FA/FB (Start) and FC (Stop) from the master drive
    // the engine's Transport state, not just the tempo display.
    juce::ToggleButton syncChaseTransportToggle { "Chase transport (Start/Stop)" };

    // MIDI Clock OUTPUT: Dusk Studio as master. Picker selects one of the
    // engine's MIDI output ports; toggle enables F8 + FA/FC emission.
    juce::Label    syncOutputLabel { {}, "MIDI Sync Output" };
    DuskComboBox syncOutputCombo;
    juce::ToggleButton syncEmitClockToggle { "Emit clock (Dusk Studio as master)" };

    // MTC slave: chase the master's absolute SMPTE time. When on AND
    // the receiver is rolling, the engine locks the playhead to the
    // master's frame count on rising edge + freewheels with re-locate.
    juce::ToggleButton mtcChaseToggle { "Chase transport from MTC" };

    // MTC master emission. Toggle = emit QF + full-frame sysex on the
    // existing Sync Output. Frame-rate dropdown selects what we encode.
    juce::ToggleButton mtcEmitToggle { "Emit MTC (Dusk Studio as master)" };
    juce::Label        mtcEmitFrameRateLabel { {}, "MTC frame rate" };
    DuskComboBox     mtcEmitFrameRateCombo;

    // Mackie Control Universal controller pair. One MIDI input (faders,
    // buttons, encoders) + one MIDI output (motor faders, LEDs, LCD,
    // timecode display, meters). Selected device identifier persists in
    // session.mcu.*; ephemeral state (bank, selected channel, assign
    // mode) lives on Session::mcu as atomics.
    juce::Label    mcuInputLabel  { {}, "MCU Control Surface Input" };
    DuskComboBox mcuInputCombo;
    juce::Label    mcuOutputLabel { {}, "MCU Control Surface Output" };
    DuskComboBox mcuOutputCombo;
    void populateMcuInputCombo();
    void populateMcuOutputCombo();
    void applyMcuInputChange();
    void applyMcuOutputChange();

    // Central MIDI bindings audit / cleanup. Per-target right-click is
    // still the primary add path; this modal lists everything in one
    // place so the user can review + remove without hunting controls.
    juce::TextButton midiBindingsButton { "MIDI Bindings..." };
    EmbeddedModal    midiBindingsModal;
    EmbeddedModal    selfTestModal;

    juce::Label  uiScaleLabel  { {}, "UI scale" };
    juce::Slider uiScaleSlider;
    juce::Label  uiScaleHint;
    bool         uiScaleDragging = false;

    // Scan-on-startup: when on, every app launch synchronously scans
    // every installed plugin format and refreshes the cached
    // KnownPluginList. Backed by AppConfig (per-machine), independent
    // of session. Validation: log line in stderr after MainComponent
    // wires the engine, showing added / total counts.
    juce::ToggleButton scanOnStartupToggle { "Scan plugins on startup" };

    // Section header labels. Each marks a logical group of rows in the
    // settings panel; resized() captures the Y of each section's top
    // edge so paint() can draw a thin horizontal separator between
    // groups.
    juce::Label audioSectionLabel         { {}, "Audio" };
    juce::Label controlSurfaceSectionLabel{ {}, "Control Surface" };
    juce::Label midiBindingsSectionLabel  { {}, "MIDI Bindings" };
    juce::Label midiSyncSectionLabel      { {}, "MIDI Sync" };
    juce::Label generalSectionLabel       { {}, "General" };
    juce::Label advancedSectionLabel      { {}, "Advanced" };
    juce::ToggleButton tapeStripExpandedToggle { "Expand tape strip by default" };

    // Separator Y positions captured during resized() and drawn by
    // paint() as a thin horizontal rule between section groups.
    std::vector<int> separatorYs;

#if defined(__linux__)
    void applyPeriodsChange();
#endif
    void applyOversamplingChange();
    void applyUiScaleChange();
    void applyRescan();
    void openSelfTest();
    void populateSyncSourceCombo();
    void applySyncSourceChange();
    void populateSyncOutputCombo();
    void applySyncOutputChange();
};
} // namespace duskstudio

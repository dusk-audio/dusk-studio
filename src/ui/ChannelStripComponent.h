#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <memory>
#include "CompMeterStrip.h"
#include "EmbeddedModal.h"
#include "DuskComboBox.h"
#include "../session/Session.h"

namespace duskstudio
{
class AudioEngine;

class ChannelStripComponent final : public juce::Component,
                                       private juce::Timer,
                                       private juce::ChangeListener
{
public:
    ChannelStripComponent (int trackIndex, Track& trackRef, Session& sessionRef,
                            class PluginSlot& slotRef, AudioEngine& engineRef);
    ~ChannelStripComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

    // Wired to TapeStrip selection so A/S/X target the just-touched strip
    // even when no region was clicked.
    std::function<void (int trackIndex)> onTrackFocusRequested;

    // Fader-group drag. Captured at onDragStart for every track in this
    // group, then on each onValueChange the delta against this strip's
    // anchor is added to each peer's anchor and stored in faderDb.
    // Cleared at onDragEnd. Peers' 30 Hz timer pushes faderDb back to
    // their slider.
    float                            faderDragAnchorDb = 0.0f;
    std::array<float, Session::kNumTracks> peerAnchorsDb {};
    std::array<bool,  Session::kNumTracks> peerActive    {};

    // Hides inline EQ + COMP, swaps in popup-launcher buttons. Used
    // when TIMELINE expands so fader / bus / meters / M-S-Ø stay
    // visible.
    void setCompactMode (bool compact);
    bool isCompactMode() const noexcept { return compactMode; }

    // Swaps the input/IN/ARM/PRINT row at the top for 4 AUX send knobs.
    void setMixingMode (bool mixing);
    bool isMixingMode() const noexcept { return mixingMode; }

private:
    void timerCallback() override;
    // AudioEngine fires after refreshMidiInputs rebuilds the device
    // bank. Repopulate dropdowns + re-resolve via saved identifier so
    // a still-present device stays selected even if its index changed.
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void rebuildMidiInputDropdown();
    void rebuildMidiOutputDropdown();

    int trackIndex;
    Track& track;
    Session& session;
    class PluginSlot& pluginSlot;
    AudioEngine& engine;
    std::array<juce::uint32, ChannelStripParams::kNumBuses> lastBusColours {};
    float displayedGrDb = 0.0f;
    float displayedInputDb = -100.0f;
    float inputPeakHoldDb = -100.0f;
    int   inputPeakHoldFrames = 0;
    float displayedInputRDb = -100.0f;
    float inputPeakHoldRDb  = -100.0f;
    int   inputPeakHoldRFrames = 0;

    juce::Label nameLabel;

    // Right-click context menu — used to be only via strip-body
    // right-click (undiscoverable).
    struct PluginSlotButton final : public juce::TextButton
    {
        using juce::TextButton::TextButton;
        std::function<void(const juce::MouseEvent&)> onRightClick;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && onRightClick)
            {
                onRightClick (e);
                return;   // skip base — don't want a stuck "down" state.
            }
            juce::TextButton::mouseDown (e);
        }
    };
    PluginSlotButton pluginSlotButton { "+ Plugin" };
    juce::String     lastSlotName;

    juce::Rectangle<int> eqArea, compArea;

    juce::Slider hpfKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider lpfKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label  hpfLabel;
    juce::Label  lpfLabel;
    // Small EQ-type toggle parked between HM and LM rows. Mid-strip so
    // the type cue reads even when the eye is on band rows.
    juce::TextButton eqTypeChip { "E" };

    // Same grammar as the COMP header. Left = toggle eqEnabled. Right
    // = type picker. Label stays "EQ" regardless of mode.
    class CompHeaderButton;   // defined in .cpp (shared with COMP)
    std::unique_ptr<CompHeaderButton> eqHeaderBtn;
    struct BandRow
    {
        std::unique_ptr<juce::Slider> gain;
        std::unique_ptr<juce::Slider> freq;
        // populated only for mid (HM/LM) bell bands; nullptr for HF/LF shelves.
        std::unique_ptr<juce::Slider> q;
        juce::Label labelLeft, labelRight;
        juce::Label rowLabel;
        juce::Label qLabel;
    };
    std::array<BandRow, 4> eqRows;

    // Holds the union of all three modes' params; shows only the
    // current mode. Illumination relies on LookAndFeel reading
    // getToggleState — we do NOT setClickingTogglesState(true) (that
    // would flip on every click and detach visual from compEnabled).
    // onClick opens the mode menu; compEnabled mutated separately.
    std::unique_ptr<CompHeaderButton> compModeButton;

    // Opto (LA-2A): peak-red + gain + LIMIT.
    juce::Slider     optoPeakRedKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     optoGainKnob    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label      optoPeakRedLabel, optoGainLabel;
    juce::TextButton optoLimitButton { "LIMIT" };

    // FET (1176): input + output + attack + release + ratio (5-step).
    juce::Slider     fetInputKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetOutputKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetAttackKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetReleaseKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetRatioKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label      fetInputLabel, fetOutputLabel, fetAttackLabel, fetReleaseLabel, fetRatioLabel;

    // VCA (classic): threshold via meter-strip drag handle (not a
    // knob); ratio + attack + release + output as knobs.
    juce::Slider     vcaRatioKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     vcaAttackKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     vcaReleaseKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     vcaOutputKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };

    juce::Label      vcaRatioLabel, vcaAttackLabel, vcaReleaseLabel, vcaOutputLabel;

    std::unique_ptr<CompMeterStrip> compMeter;

    // CompMeterStrip hoisted into a slim column alongside the fader
    // (handle + IN bar + GR LED) so the COMP section shows only the
    // knob grid.
    bool usesFaderThresholdLayout() const { return true; }

    std::array<std::unique_ptr<juce::TextButton>, ChannelStripParams::kNumBuses> busButtons;

    juce::Slider panKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label  panLabel;
    juce::Slider faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::Rectangle<int> inputMeterArea;
    juce::Rectangle<int> meterScaleArea;
    juce::Rectangle<int> grScaleArea;
    juce::Label inputPeakLabel;
    juce::Label grPeakLabel;
    juce::Label grReadoutLabel;
    // Slider runs NoTextBox so the cap at min value doesn't overlap
    // the textbox area.
    juce::Label faderValueLabel;
    juce::Label threshMeterLabel;
    juce::TextButton muteButton    { "M" };
    juce::TextButton soloButton    { "S" };
    juce::TextButton phaseButton   { juce::CharPointer_UTF8 ("\xc3\x98") };  // Ø

    // 3c-i wires Off/Read; Write/Touch surfaced in 3c-ii. Label mirrors
    // current mode.
    juce::TextButton autoModeButton { "OFF" };
    void showAutoModeMenu();
    void setAutoMode (AutomationMode mode);
    void refreshAutoModeButton();
    // Strict ascending order — evaluateLane's binary search depends on
    // it. Same-sample writes coalesce; loop wraparound truncates future
    // points.
    void captureWritePoint (AutomationParam param, float denormValue);
    // Gated by small delta so the timer doesn't churn setValue when
    // manual mode just mirrors the user's setpoint.
    float displayedLiveFaderDb = 0.0f;
    float displayedLivePan = 0.0f;
    std::array<float, ChannelStripParams::kNumAuxSends> displayedLiveAuxSendDb {};
    juce::TextButton armButton     { "ARM" };
    juce::TextButton monitorButton { "IN"  };
    juce::TextButton printButton   { "PRINT" };
    DuskComboBox   modeSelector;          // Mono / Stereo / MIDI
    DuskComboBox   inputSelector;
    DuskComboBox   inputSelectorR;
    DuskComboBox   midiInputSelector;
    DuskComboBox   midiChannelSelector;
    DuskComboBox   midiOutputSelector;

    // Compact summary button replacing the inline mode/input/midi rows
    // (~60 px saved). Opens InputConfigPanel popup; the 6 dropdowns
    // above live as members so the popup re-parents them on open
    // without losing wiring / state.
    juce::TextButton ioConfigButton;
    void openIoConfigPopup();
    void refreshIoConfigButton();
    // PRINT only commits post-effects audio. MIDI tracks render audio
    // at playback time, not at capture, so PRINT is a no-op. Grey out
    // the button + swap the tooltip in MIDI mode. Called from the ctor
    // and from applyMode().
    void refreshPrintButtonForMode();

    // Re-renders one aux send's value label (e.g. after a pre/post
    // toggle from the right-click menu). Reads the current dB +
    // pre-fader bool from the session atoms and pushes the formatted
    // text into auxKnobLabels[auxIdx].
    void refreshAuxSendLabel (int auxIdx);
    juce::Component::SafePointer<juce::CallOutBox> activeIoBox;
    // Repainted by the 30 Hz timer when engine sets track.midiActivity
    // (clear-on-read).
    struct MidiActivityLed : juce::Component
    {
        bool lit = false;
        void paint (juce::Graphics& g) override;
    };
    MidiActivityLed midiActivityLed;

    // PRE/POST toggle per channel-per-aux is auxSendPreFader[i].
    // Index labels coloured per aux so the user can identify N at a
    // glance without the AUX-page tab strip.
    std::array<juce::Label, ChannelStripParams::kNumAuxSends> auxIndexLabels;
    std::array<std::unique_ptr<juce::Slider>, ChannelStripParams::kNumAuxSends> auxKnobs;
    std::array<juce::Label,                  ChannelStripParams::kNumAuxSends> auxKnobLabels;
    bool mixingMode = false;
    juce::Rectangle<int> auxRowArea;

    void onHpfKnobChanged();
    void onLpfKnobChanged();
    void onInputSelectorChanged();
    void onTrackModeChanged();
    void refreshInputSelectorVisibility();
    void showColourMenu();
    void applyTrackColour (juce::Colour c);
    void setCompMode (int modeIndex);
    // Cheap: button text + illumination. Called every tick + after any
    // compMode / compEnabled mutation.
    void refreshCompModeButtonState();
    // Heavy: shows current mode's knobs + hides others. Only on real
    // mode change or visibility flip — NOT from the 30 Hz timer.
    void refreshCompKnobVisibility();
    void showCompModeMenu();
    void showEqTypeMenu();
    void armCompOnUserEdit();
    // Right-click context menu uses this since the button itself is
    // the mode picker.
    void setCompEnabled (bool enabled);

    // useChooser=true: shows 3-button Add Insert chooser (Hardware /
    // Soundfont / Plugin) first. false: jump to plugin list (used by
    // Replace plugin... where the user already committed to plugin).
    void openPluginPicker (bool useChooser = true);
    void unloadPluginSlot();
    void refreshPluginSlotButton();

    // Flips insertMode to Hardware and shows HardwareInsertEditor in
    // an EmbeddedModal owned by this strip.
    void openHardwareInsertEditor();
    EmbeddedModal hardwareInsertModal;
    void showPluginSlotMenu();
    void togglePluginEditor();
    void openPluginEditor();
    void closePluginEditor();
    std::unique_ptr<juce::FileChooser> activePluginChooser;
    // Editor hosted inline as an EmbeddedModal (centred, dim backdrop,
    // click-out / Esc dismiss). showBorrowed doesn't own the body so
    // GL / Cairo / native resources survive close/reopen cycles. Both
    // generic-fallback and native editors share this modal — X11 sub-
    // window reparents into the modal wrapper without its own top-
    // level peer.
    EmbeddedModal pluginEditorModal;
    std::unique_ptr<juce::AudioProcessorEditor> pluginEditor;
    juce::AudioProcessor* pluginEditorOwner = nullptr;

   #if JUCE_LINUX && DUSKSTUDIO_HAS_OOP_PLUGINS
    // OOP: child plugin's X11 Window wrapped in XEmbedComponent fed
    // to PluginEditorWindow as the body. Lifetime matches pluginEditor.
    std::unique_ptr<juce::XEmbedComponent> remoteEditorEmbed;
   #endif
   #if DUSKSTUDIO_HAS_OOP_PLUGINS && ! JUCE_LINUX
    // Windows OOP via SetParent. macOS keeps null and falls through to
    // floating-window path. ~ForeignHwndEmbed re-parents back to
    // desktop so OOP host can hide cleanly.
    std::unique_ptr<juce::Component> remoteForeignEmbed;
   #endif

    bool isPluginEditorOpen() const noexcept;

public:
    // Public so host (MainComponent / ConsoleView) can force-close all
    // editors on shutdown BEFORE the chain destructs. Tearing down a
    // plugin's native X11 window during Mutter's cascade-shutdown of
    // the main window race-crashes the compositor on Linux/Wayland.
    void dropPluginEditor();

private:

    bool compactMode = false;
    juce::TextButton eqCompactButton   { "EQ" };
    juce::TextButton compCompactButton { "COMP" };
    juce::TextButton auxCompactButton  { "AUX" };
    juce::Component::SafePointer<juce::CallOutBox> activeEqBox;
    juce::Component::SafePointer<juce::CallOutBox> activeCompBox;
    juce::Component::SafePointer<juce::CallOutBox> activeAuxBox;
    void openEqEditorPopup();
    void openCompEditorPopup();
    void openAuxEditorPopup();
    void setAuxSectionVisible (bool visible);

    // Translucent shade on the top-level while either popup is open.
    // Removed by timerCallback once both are gone.
    std::unique_ptr<class DimOverlay> activeDimOverlay;
    void attachDimOverlay();
    void detachDimOverlay();

    void setEqSectionVisible (bool visible);
    void setCompSectionVisible (bool visible);
};
} // namespace duskstudio

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <memory>
#include "CompMeterStrip.h"
#include "EmbeddedModal.h"
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

    // Click-to-focus hook. Fired when the user clicks anywhere on the
    // strip (or any of its children, via the wide click-target). The
    // host (MainComponent) wires this to the TapeStrip selection so
    // keyboard shortcuts (A / S / X) target the strip the user just
    // touched, even when no region has been clicked.
    std::function<void (int trackIndex)> onTrackFocusRequested;

    // Fader-group drag state. Captured at onDragStart for every track
    // in the same group as this strip, then on each onValueChange the
    // delta against this strip's anchor is added to each peer's anchor
    // and stored in their faderDb atom. Cleared at onDragEnd. The peer
    // ChannelStripComponents pick up the change via their existing
    // 30 Hz timer that polls faderDb and pushes it to their slider.
    float                            faderDragAnchorDb = 0.0f;
    std::array<float, 16>            peerAnchorsDb {};   // key = trackIndex
    std::array<bool,  16>            peerActive    {};   // true = peer in group

    // Compact mode hides the inline EQ + COMP controls and replaces each
    // section with a single small button that opens the corresponding
    // editor as a modal popup. Used when the SUMMARY (tape-strip) view is
    // expanded, so the fader / bus assigns / meters / M-S-Ø stay visible
    // even on a screen the SUMMARY ate half of.
    void setCompactMode (bool compact);
    bool isCompactMode() const noexcept { return compactMode; }

    // Mixing-stage flag - swaps the input/IN/ARM/PRINT block at the top of
    // the strip for a row of 4 AUX send knobs (one per aux bus). The aux
    // strips host reverb / delay / etc. plugin chains; these knobs are how
    // each channel feeds them.
    void setMixingMode (bool mixing);
    bool isMixingMode() const noexcept { return mixingMode; }

private:
    void timerCallback() override;
    // Fired by AudioEngine after refreshMidiInputs() rebuilds the device
    // bank. Repopulates this strip's MIDI input dropdown so the new
    // device list is visible and re-resolves the selection via the
    // saved identifier (so a device that's still present stays selected
    // even if its index in the list changed).
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void rebuildMidiInputDropdown();
    void rebuildMidiOutputDropdown();

    int trackIndex;
    Track& track;
    Session& session;
    class PluginSlot& pluginSlot;  // owned by the AudioEngine's ChannelStrip
    AudioEngine& engine;            // for transport playhead + isPlaying queries
                                    // during Write capture (3c-ii)
    std::array<juce::uint32, ChannelStripParams::kNumBuses> lastBusColours {};  // for change detection in timerCallback
    float displayedGrDb = 0.0f;     // smoothed GR for the comp meter
    float displayedInputDb = -100.0f; // smoothed input level (L) for the level meter
    float inputPeakHoldDb = -100.0f;  // brief peak hold marker (L)
    int   inputPeakHoldFrames = 0;
    float displayedInputRDb = -100.0f;   // R-channel smoothed level (stereo mode)
    float inputPeakHoldRDb  = -100.0f;
    int   inputPeakHoldRFrames = 0;

    juce::Label nameLabel;

    // Per-channel insert plugin slot. Sits between the input row and the
    // EQ section. Click-left to load (or replace) a plugin via file chooser;
    // click-right to unload. Plugin name is shown on the button when loaded.
    // Custom subclass that adds a right-click context menu so the user can
    // change/remove the loaded plugin without having to right-click the
    // strip body (which used to be the only path - undiscoverable).
    struct PluginSlotButton final : public juce::TextButton
    {
        using juce::TextButton::TextButton;
        std::function<void(const juce::MouseEvent&)> onRightClick;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && onRightClick)
            {
                onRightClick (e);
                return;   // do NOT pass to base - we don't want a stuck "down" state
            }
            juce::TextButton::mouseDown (e);
        }
    };
    PluginSlotButton pluginSlotButton { "+ Plugin" };
    juce::String     lastSlotName;          // for change detection in timerCallback

    juce::Rectangle<int> eqArea, compArea;

    juce::Slider hpfKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label  hpfLabel;

    juce::Label  eqHeaderLabel;
    juce::TextButton eqTypeButton { "B" };
    struct BandRow
    {
        std::unique_ptr<juce::Slider> gain;
        std::unique_ptr<juce::Slider> freq;
        std::unique_ptr<juce::Slider> q;     // populated for bell-only mid bands (HM/LM); nullptr for shelf bands (HF/LF)
        juce::Label labelLeft, labelRight;
        juce::Label rowLabel;
        juce::Label qLabel;   // "Q" header above the Q knob (bell bands only)
    };
    std::array<BandRow, 4> eqRows;

    // COMP section. UniversalCompressor exposes a different control set per
    // mode (Opto/FET/VCA model real hardware), so the strip holds the union
    // of all three and shows only the ones for the current mode. The header
    // button shows the active mode (OPTO/FET/VCA) and opens a popup picker
    // on click. It also illuminates (background fills gold) when the comp
    // is engaged - touching any knob auto-engages the comp.
    //
    // NOTE: illumination relies on the LookAndFeel reading getToggleState()
    // independently of setClickingTogglesState. We do NOT call
    // setClickingTogglesState(true) - that would flip the state on every
    // click and detach the visual state from the underlying compEnabled
    // atom. The button's onClick opens the mode menu; compEnabled is
    // mutated separately via setCompEnabled / armCompOnUserEdit.
    juce::TextButton compModeButton  { "OPTO" };

    // Opto (LA-2A): peak-reduction knob + gain knob + LIMIT toggle.
    juce::Slider     optoPeakRedKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     optoGainKnob    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label      optoPeakRedLabel, optoGainLabel;
    juce::TextButton optoLimitButton { "LIMIT" };

    // FET (1176): input + output + attack + release + ratio knob (5-step).
    juce::Slider     fetInputKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetOutputKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetAttackKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetReleaseKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetRatioKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label      fetInputLabel, fetOutputLabel, fetAttackLabel, fetReleaseLabel, fetRatioLabel;

    // VCA (classic): threshold via meter-strip drag handle (NOT a knob);
    // ratio + attack + release + output as knobs.
    juce::Slider     vcaRatioKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     vcaAttackKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     vcaReleaseKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     vcaOutputKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label      vcaRatioLabel, vcaAttackLabel, vcaReleaseLabel, vcaOutputLabel;

    std::unique_ptr<CompMeterStrip> compMeter;

    std::array<std::unique_ptr<juce::TextButton>, ChannelStripParams::kNumBuses> busButtons;

    juce::Slider panKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label  panLabel;
    juce::Slider faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::Rectangle<int> inputMeterArea;
    juce::Rectangle<int> meterScaleArea;
    juce::Label inputPeakLabel;
    juce::Label grPeakLabel;       // GR readout (dB), sits to the right of inputPeakLabel
    juce::Label threshMeterLabel;  // "THR" header above CompMeterStrip's drag handle
    juce::TextButton muteButton    { "M" };
    juce::TextButton soloButton    { "S" };
    juce::TextButton phaseButton   { juce::CharPointer_UTF8 ("\xc3\x98") };  // Ø - phase invert

    // Automation mode cycle: OFF (dark) <-> READ (green). Phase 3c-i wires
    // only Off/Read; the underlying enum already covers Write/Touch which
    // 3c-ii will surface here. The label updates to mirror the current
    // mode (e.g. "OFF", "READ"). Right-click reserved for future per-param
    // mode picker (also 3c-ii).
    juce::TextButton autoModeButton { "OFF" };
    void showAutoModeMenu();
    void setAutoMode (AutomationMode mode);
    void refreshAutoModeButton();
    // Append (normalized) `denormValue` to track.automationLanes[param]
    // at the current transport playhead. Maintains strict ascending
    // ordering so evaluateLane's binary search stays correct. Same-
    // sample writes coalesce; loop wraparound truncates future points.
    void captureWritePoint (AutomationParam param, float denormValue);
    // Last live fader dB rendered into the slider, polled in timerCallback
    // from track.strip.liveFaderDb. We avoid driving setValue every tick
    // by gating on a small delta - prevents UI churn when manual mode just
    // mirrors the user's setpoint and nothing is moving.
    float displayedLiveFaderDb = 0.0f;
    // Same idea for the pan knob - polled from track.strip.livePan.
    float displayedLivePan = 0.0f;
    // And for each aux send knob - polled from track.strip.liveAuxSendDb[i].
    std::array<float, ChannelStripParams::kNumAuxSends> displayedLiveAuxSendDb {};
    juce::TextButton armButton     { "ARM" };
    juce::TextButton monitorButton { "IN"  };
    juce::TextButton printButton   { "PRINT" };  // print EQ/comp to recording
    juce::ComboBox   modeSelector;          // Mono / Stereo / MIDI
    juce::ComboBox   inputSelector;         // mono / stereo-L / (hidden in MIDI)
    juce::ComboBox   inputSelectorR;        // stereo-R (visible only in stereo mode)
    juce::ComboBox   midiInputSelector;     // MIDI input port (visible only in MIDI mode)
    juce::ComboBox   midiChannelSelector;   // Omni / Ch 1..16 filter (MIDI mode only)
    juce::ComboBox   midiOutputSelector;    // External MIDI output port (MIDI mode only)

    // Compact header that replaces the inline mode/input/midi rows. Shows a
    // one-line summary of the track's I/O (e.g. "Mono · In 1" or "MIDI Ch 5
    // → Diva") and opens an InputConfigPanel popup when clicked. Saves ~60
    // px vertical per strip; the 6 ComboBoxes above live as members so the
    // popup can re-parent them on open and they keep their wiring + state.
    juce::TextButton ioConfigButton;
    void openIoConfigPopup();
    void refreshIoConfigButton();
    juce::Component::SafePointer<juce::CallOutBox> activeIoBox;
    // Small painted dot, repainted by the strip's existing 30 Hz timer when
    // the engine sets track.midiActivity (clear-on-read). Sits next to the
    // MIDI selectors when the track is in MIDI mode.
    struct MidiActivityLed : juce::Component
    {
        bool lit = false;
        void paint (juce::Graphics& g) override;
    };
    MidiActivityLed midiActivityLed;

    // Aux send knobs (visible only in Mixing stage). Each sends a copy of
    // the channel signal into the matching AUX strip's plugin chain. The
    // PRE/POST tap point is per-channel-per-aux (auxSendPreFader[i]) and
    // gets its UI in Phase D.
    // One small index label ("1".."4") per aux send, coloured per
    // kAuxColours so the user can tell at a glance which knob feeds AUX
    // N without looking up the colour against the AUX-page tab strip.
    std::array<juce::Label, ChannelStripParams::kNumAuxSends> auxIndexLabels;
    std::array<std::unique_ptr<juce::Slider>, ChannelStripParams::kNumAuxSends> auxKnobs;
    std::array<juce::Label,                  ChannelStripParams::kNumAuxSends> auxKnobLabels;
    bool mixingMode = false;
    juce::Rectangle<int> auxRowArea;

    void onHpfKnobChanged();
    void onInputSelectorChanged();
    void onTrackModeChanged();
    void refreshInputSelectorVisibility();
    void showColourMenu();
    void applyTrackColour (juce::Colour c);
    void refreshEqTypeButton();
    void setCompMode (int modeIndex);
    // Cheap state-only refresh: button text + toggle illumination. Called
    // every timer tick + after any compMode/compEnabled mutation.
    void refreshCompModeButtonState();
    // Heavy visibility refresh: shows the active mode's per-mode knobs +
    // hides the other modes'. Only invoked after a real mode change or
    // when the section's visibility flips - NOT from the 30 Hz timer.
    void refreshCompKnobVisibility();
    void showCompModeMenu();
    void armCompOnUserEdit();
    // Toggle comp on/off + refresh mode-button illumination. Used by the
    // right-click context menu since the button itself is the mode picker.
    void setCompEnabled (bool enabled);

    // Plugin slot UX. Delegates to pluginpicker::openPickerMenu, passing
    // PluginKind based on track mode (Instruments for Midi, Effects for
    // Mono/Stereo). The shared helper handles the menu, the scan dialog,
    // and the browse-for-file fallback.
    void openPluginPicker();
    void unloadPluginSlot();
    void refreshPluginSlotButton();

    // External Hardware Insert UX. The picker calls back into
    // openHardwareInsertEditor on selection; that flips the strip's
    // insertMode to Hardware (via AudioEngine::getChannelStrip(t)
    // .insertMode.store) and shows HardwareInsertEditor inside an
    // EmbeddedModal owned by this strip.
    void openHardwareInsertEditor();
    EmbeddedModal hardwareInsertModal;
    // Right-click context menu for the plugin slot button. Mirrors the
    // plugin items inside showColourMenu() but is reachable directly from
    // the slot button so users don't have to discover the strip-body
    // right-click. Shown by PluginSlotButton::onRightClick.
    void showPluginSlotMenu();
    void togglePluginEditor();              // open if closed, close if open
    void openPluginEditor();
    void closePluginEditor();
    std::unique_ptr<juce::FileChooser> activePluginChooser;
    // Plugin editor uses an EmbeddedModal (centred overlay + dim backdrop on
    // the parent, Esc / click-outside dismiss) instead of a native top-level
    // DialogWindow. Lifetime is tied to this strip - the modal closes when
    // the strip is destroyed, before the underlying PluginSlot tears down.
    // Plugin editors need a NATIVE window peer to render correctly -
    // many plugins (e.g. MininnDrum, anything VSTGUI-based, anything
    // using its own GL context) draw a white / blank surface when
    // hosted as a child of another JUCE Component because the
    // Component shares its parent's peer. So plugin editors are the
    // explicit exception to the embedded-modal rule (memory:
    // feedback_modal_preference) and get a real top-level window with
    // its own peer.
    //
    // The window is rebuilt on every open (cheap) but the inner
    // AudioProcessorEditor is cached in `pluginEditor` so the plugin's
    // own GL / Cairo / native resources aren't torn down per close.
    // That's what stops Diva and similar plugins from crashing the
    // compositor on rapid open/close cycles.
    class PluginEditorWindow;
    std::unique_ptr<PluginEditorWindow> pluginEditorWindow;
    std::unique_ptr<juce::AudioProcessorEditor> pluginEditor;
    juce::AudioProcessor* pluginEditorOwner = nullptr;
    // Generic-fallback editors (plugins with no native UI) host inside
    // the main window as an EmbeddedModal so combobox / popup peers
    // share the main wl_surface — avoids the mixed X11/Wayland popup
    // breakage that happens when a JUCE-native editor is wrapped in an
    // X11 PluginEditorWindow.
    EmbeddedModal pluginGenericEditorModal;

   #if JUCE_LINUX && DUSKSTUDIO_HAS_OOP_PLUGINS
    // OOP-mode editor embedding. The plugin's editor lives in the
    // dusk-studio-plugin-host child; we wrap its X11 Window ID in a JUCE
    // XEmbedComponent and feed THAT into PluginEditorWindow as the
    // body. Lifetime mirrors `pluginEditor` (in-process counterpart):
    // built lazily on first open, kept across close/reopen cycles so
    // the child's GUI resources aren't torn down per close, dropped
    // when the slot is unloaded.
    std::unique_ptr<juce::XEmbedComponent> remoteEditorEmbed;
   #endif
   #if DUSKSTUDIO_HAS_OOP_PLUGINS && ! JUCE_LINUX
    // Windows OOP editor embedding via SetParent. macOS keeps this
    // null and falls through to the floating-window path. Lifetime
    // tied to pluginEditorWindow — ~ForeignHwndEmbed re-parents the
    // child HWND back to desktop so the OOP host can hide it cleanly.
    std::unique_ptr<juce::Component> remoteForeignEmbed;
   #endif

    bool isPluginEditorOpen() const noexcept;

public:
    // Public so the host (MainComponent / ConsoleView) can force-close
    // all plugin editor windows on app shutdown BEFORE the channel-strip
    // chain destructs. Tearing down a plugin editor's native X11 window
    // during Mutter's own cascade-shutdown of our main window race-
    // crashes the compositor on Linux/Wayland; closing them first, with
    // the audio engine still alive, side-steps the race.
    void dropPluginEditor();

private:

    // Compact-mode plumbing.
    bool compactMode = false;
    juce::TextButton eqCompactButton   { "EQ" };
    juce::TextButton compCompactButton { "COMP" };
    // EQ + Comp use CallOutBox (in-window overlay, click-outside-to-dismiss
    // with the click consumed so the trigger button doesn't bounce-reopen).
    juce::Component::SafePointer<juce::CallOutBox> activeEqBox;
    juce::Component::SafePointer<juce::CallOutBox> activeCompBox;
    void openEqEditorPopup();
    void openCompEditorPopup();

    // Translucent shade attached to the top-level component while either
    // popup is open. timerCallback removes it once both popups are gone.
    std::unique_ptr<class DimOverlay> activeDimOverlay;
    void attachDimOverlay();
    void detachDimOverlay();

    // Apply visibility to every EQ child / every COMP child in one shot -
    // the strip has a lot of controls split across the two sections so we
    // gather them here rather than scattering setVisible calls.
    void setEqSectionVisible (bool visible);
    void setCompSectionVisible (bool visible);
};
} // namespace duskstudio

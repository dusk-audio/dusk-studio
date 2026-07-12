#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "../session/Session.h"
#include "DuskComboBox.h"

namespace duskstudio
{
class PluginSlot;
class AuxLaneStrip;
class AudioEngine;
class HardwareInsertEditor;
class ClapPluginEditorComponent;

// AUX return lane. Three-column layout: aux-return strip (name, mute,
// return fader, output meter) on the left, plugin slot in the center,
// send-source panel showing every channel's send-to-this-lane level on
// the right. The plugin editor embeds inline in the center area below
// the slot header (DuskStudioApp forces the main window peer to X11 on
// Linux so OOP plugin X11 sub-windows can reparent into the lane).
class AuxLaneComponent final : public juce::Component, private juce::Timer
{
public:
    AuxLaneComponent (AuxLane& laneRef, AuxLaneStrip& strip, int laneIndex,
                       AudioEngine& engineRef);
    ~AuxLaneComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Shutdown: tear down this lane's native editors (CLAP, LV2, VST3) now, while
    // the main peer + message loop are alive. CLAP/LV2 leak their plugin UIs (a
    // foreign toolkit's destructor can hang on the way out) and every kind closes
    // its own X11 Display, which hangs if deferred to the destructor cascade. See
    // MainComponent::beginSafeShutdown phase 4.
    void dropAllNativeEditors();

    void childBoundsChanged (juce::Component* child) override;
    void mouseDown (const juce::MouseEvent&) override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

    // Stage switches flip AuxView's OWN visible flag; the lanes' flags don't
    // change, so their visibilityChanged never fires. AuxView forwards its
    // change here so the active lane (re)builds its inline editors on first
    // show and hides them when the stage goes away.
    void refreshEditorsForShowState();

    static constexpr int kStripWidth      = 150;
    static constexpr int kSendPanelWidth  = 280;
    static constexpr int kColumnGap       = 8;
    static constexpr int kSlotHeaderH     = 24;

private:
    class SendSourcePanel;

    void timerCallback() override;
    void rebuildSlots();
    void openPickerForSlot (int slotIdx);
    void openHardwareInsertEditor (int slotIdx);
    void unloadSlot (int slotIdx);
    void toggleEditorForSlot (int slotIdx);
    void refreshSlotControls (int slotIdx);
    void attachEditorForSlot (int slotIdx);
    void detachEditorForSlot (int slotIdx);
    void attachHardwareInsertForSlot (int slotIdx);
    void detachHardwareInsertForSlot (int slotIdx);
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    void loadNativeClapForSlot (int slotIdx, const juce::File& clapFile,
                                const juce::String& pluginId = {});   // Linux-only
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    void loadNativeLv2ForSlot (int slotIdx, const juce::File& bundleDir,
                               const juce::String& pluginId = {});   // Linux-only
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    void loadNativeVst3ForSlot (int slotIdx, const juce::File& vst3File,
                                const juce::String& pluginId = {});   // Linux-only
#endif
    // Stubbed (no-op body) off Linux so the many callers don't each need a guard.
    void detachClapEditorForSlot (int slotIdx);
    void detachLv2EditorForSlot (int slotIdx);
    void detachVst3EditorForSlot (int slotIdx);
    void hideEditorsKeepingAlive();
    void layoutEditorForSlot (int slotIdx);
    void scheduleEditorRefits (int slotIdx);

    juce::Rectangle<int> getStripArea() const noexcept;
    juce::Rectangle<int> getCenterArea() const noexcept;
    juce::Rectangle<int> getSendPanelArea() const noexcept;

    void showAutoModeMenu();
    void setAutoMode (AutomationMode m);
    void captureWritePoint (AutomationParam param, float denormValue);

    // Cue/headphone output-pair picker. Rebuilt from the live device's output
    // channels (so it tracks outputs the user enables in Audio settings).
    void populateOutputPairCombo();

    AuxLane& lane;
    AuxLaneStrip& strip;
    AudioEngine& engine;
    int laneIndex;

    juce::Label   nameLabel;
    juce::Slider  returnFader { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::TextButton muteButton { "M" };
    juce::TextButton autoModeButton { "Off" };
    DuskComboBox  outputPairCombo;

    // Active-output channel mask the combo was last built for; the timer
    // rebuilds it when the device's active-output set changes. A bitmask (not
    // just a set-bit count) so swapping which pair is active - same count,
    // different channels - still triggers a repopulate.
    juce::BigInteger lastOutputChannelMask;
    // Output channel count the combo was last built for. The mask alone can
    // match across two devices (both stereo -> bits 0,1) while the physical
    // output count - which drives how many pairs the menu lists - differs, so
    // a device swap with the same active mask still needs a repopulate.
    int lastOutputChannelCount { -1 };

    // Throttle motor-fader setValue() to changes >0.05 dB so the slider
    // doesn't churn every timer tick when the value is effectively
    // stable. Mirrors the per-channel pattern.
    float displayedLiveReturnLevelDb { 0.0f };

    // Top-level peer the kept-alive plugin editors were built against. A peer
    // change (fullscreen toggle recreates the X11 peer) orphans their embedded
    // windows on the dead peer, so parentHierarchyChanged tears them down to
    // force a rebuild rather than the cheap same-peer keep-alive remap.
    juce::ComponentPeer* lastSeenPeer = nullptr;

    // Repainted by timer for the return-fader meter bar.
    class StripMeter;
    std::unique_ptr<StripMeter> stripMeter;

    std::unique_ptr<SendSourcePanel> sendPanel;

    struct SlotUI
    {
        juce::TextButton openOrAddButton;
        juce::TextButton bypassButton;
        juce::TextButton removeButton;

        // Exactly one of these is attached at a time, gated by
        // strip.insertMode[i]:
        //   kInsertPlugin   -> `editor` (plugin's AudioProcessorEditor)
        //   kInsertHardware -> `hwInsertEditor` (HardwareInsertEditor panel)
        std::unique_ptr<juce::AudioProcessorEditor> editor;
        std::unique_ptr<HardwareInsertEditor>       hwInsertEditor;
        // Native CLAP editor (when strip.isNativeClapLoaded(i)); shares the lane's
        // NativeClapSlot instance. Mutually exclusive with `editor` above. Linux-only.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
        std::unique_ptr<ClapPluginEditorComponent>  clapEditor;
#endif
        // Native LV2 (suil) editor - same contract as clapEditor.
#if DUSKSTUDIO_HAS_NATIVE_LV2
        std::unique_ptr<class Lv2PluginEditorComponent> lv2Editor;
#endif
        // Native VST3 (IPlugView) editor - same contract as clapEditor.
#if DUSKSTUDIO_HAS_NATIVE_VST3
        std::unique_ptr<class Vst3PluginEditorComponent> vst3Editor;
#endif
        juce::String displayedName;
    };
    std::array<SlotUI, AuxLaneParams::kMaxLanePlugins> slots;
};
} // namespace duskstudio

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
    void childBoundsChanged (juce::Component* child) override;
    void mouseDown (const juce::MouseEvent&) override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

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
    // just a set-bit count) so swapping which pair is active — same count,
    // different channels — still triggers a repopulate.
    juce::BigInteger lastOutputChannelMask;
    // Output channel count the combo was last built for. The mask alone can
    // match across two devices (both stereo → bits 0,1) while the physical
    // output count — which drives how many pairs the menu lists — differs, so
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
        //   kInsertPlugin   → `editor` (plugin's AudioProcessorEditor)
        //   kInsertHardware → `hwInsertEditor` (HardwareInsertEditor panel)
        std::unique_ptr<juce::AudioProcessorEditor> editor;
        std::unique_ptr<HardwareInsertEditor>       hwInsertEditor;
        juce::String displayedName;
    };
    std::array<SlotUI, AuxLaneParams::kMaxLanePlugins> slots;
    std::unique_ptr<juce::FileChooser> activePluginChooser;
};
} // namespace duskstudio

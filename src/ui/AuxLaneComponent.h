#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "../session/Session.h"
#include "EmbeddedModal.h"

namespace duskstudio
{
class PluginSlot;
class AuxLaneStrip;
class AudioEngine;
class AuxEditorHost;

// AUX return lane. Three-column layout: aux-return strip (name, mute,
// return fader, output meter) on the left, plugin host area in the
// center, send-source panel showing every channel's send-to-this-lane
// level on the right. The plugin editor is hosted by an AuxEditorHost -
// a borderless X11 toplevel positioned over the center-column slot rect
// so it reads as embedded. The editor cannot be a true Component child
// on Wayland because X11 plugin windows can't reparent into a wl_surface.
class AuxLaneComponent final : public juce::Component, private juce::Timer
{
public:
    AuxLaneComponent (AuxLane& laneRef, AuxLaneStrip& strip, int laneIndex,
                       AudioEngine& engineRef);
    ~AuxLaneComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    void repositionEditorHosts();
    void setEditorHostsHidden (bool hidden);
    void closeAllPopoutsForShutdown();

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
    void createEditorHostForSlot (int slotIdx);
    void destroyEditorHostForSlot (int slotIdx);
    juce::Rectangle<int> computeSlotScreenRect (int slotIdx) const;

    juce::Rectangle<int> getStripArea() const noexcept;
    juce::Rectangle<int> getCenterArea() const noexcept;
    juce::Rectangle<int> getSendPanelArea() const noexcept;

    void showAutoModeMenu();
    void setAutoMode (AutomationMode m);
    void captureWritePoint (AutomationParam param, float denormValue);

    AuxLane& lane;
    AuxLaneStrip& strip;
    AudioEngine& engine;
    int laneIndex;

    juce::Label   nameLabel;
    juce::Slider  returnFader { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::TextButton muteButton { "M" };
    juce::TextButton autoModeButton { "Off" };

    // Throttle motor-fader setValue() to changes >0.05 dB so the slider
    // doesn't churn every timer tick when the value is effectively
    // stable. Mirrors the per-channel pattern.
    float displayedLiveReturnLevelDb { 0.0f };

    // Repainted by timer for the return-fader meter bar.
    class StripMeter;
    std::unique_ptr<StripMeter> stripMeter;

    std::unique_ptr<SendSourcePanel> sendPanel;

    struct SlotUI
    {
        juce::TextButton openOrAddButton;
        juce::TextButton bypassButton;
        juce::TextButton removeButton;

        std::unique_ptr<juce::AudioProcessorEditor> editor;
        std::unique_ptr<AuxEditorHost>              editorHost;
        juce::String displayedName;
    };
    std::array<SlotUI, AuxLaneParams::kMaxLanePlugins> slots;
    std::unique_ptr<juce::FileChooser> activePluginChooser;
    EmbeddedModal hardwareInsertModal;
};
} // namespace duskstudio

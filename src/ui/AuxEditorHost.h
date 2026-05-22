#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio
{
// Floating X11 toplevel (XWayland on Linux) that hosts an AUX-bus plugin's
// AudioProcessorEditor. Visually-embedded in-lane hosting is not achievable
// on the JUCE-wayland fork because X11 plugin sub-windows cannot reparent
// into a wl_surface and Wayland never exposes a surface's screen position
// to clients (so positioning an X11 toplevel "over" the lane fails). This
// is the same floating-window pattern ChannelStripComponent::PluginEditorWindow
// uses for channel-strip plugins - centered on the desktop, native title
// bar + close button, user drags as needed.
class AuxEditorHost final : public juce::DocumentWindow
{
public:
    // onClose fires when the user hits the title-bar close button; the
    // owner is expected to drop its unique_ptr<AuxEditorHost> in response
    // (deferred via callAsync so we don't destruct from inside our own
    // closeButtonPressed). processorForResizability is forwarded for the
    // editor's isResizable check; engineForTransport receives spacebar
    // play/stop while the editor has focus.
    AuxEditorHost (const juce::String& title,
                    juce::Component& editor,
                    juce::AudioProcessor* processorForResizability,
                    class AudioEngine* engineForTransport,
                    std::function<void()> onClose);
    ~AuxEditorHost() override;

    // Hide/show the host without destroying it. Hosted editor stays
    // attached. Used when the AUX tab swaps to MIXING / RECORDING /
    // MASTERING.
    void setHostHidden (bool hidden);

    bool keyPressed (const juce::KeyPress& k) override;
    void closeButtonPressed() override;

private:
    juce::Component::SafePointer<juce::Component> trackedEditor;
    juce::AudioProcessor* processor = nullptr;
    class AudioEngine* enginePtr = nullptr;
    std::function<void()> onCloseCallback;
};
} // namespace duskstudio

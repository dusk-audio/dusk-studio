#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio
{
// Borderless X11 top-level that hosts a plugin's AudioProcessorEditor
// visually attached to an AUX lane's slot rect. The host is a separate
// X11 toplevel because the Focal main window on Linux is a wl_surface
// and X11 plugin editors (VST3, LV2) can't reparent into a Wayland
// surface. Tracking the parent's screen rect + hiding when the AUX tab
// isn't visible reads to the user as embedded panel hosting - no
// detached popout window.
class AuxEditorHost final : public juce::DocumentWindow
{
public:
    // Body comes in as a juce::Component& so both in-process plugin
    // editors and (future) OOP XEmbedComponent wrappers can host through
    // the same path. Spacebar still toggles transport when the host has
    // focus, matching PluginEditorWindow.
    AuxEditorHost (const juce::String& title,
                    juce::Component& editor,
                    juce::AudioProcessor* processorForResizability,
                    class AudioEngine* engineForTransport);
    ~AuxEditorHost() override;

    // Move + resize the host to match an AUX slot's rect in screen
    // coordinates. If the hosted editor is resizable, it is sized to fill
    // the host; otherwise it stays at its native size and is positioned
    // centred within the host. No-op when bounds are empty.
    void setLaneScreenBounds (juce::Rectangle<int> screenRect);

    // Hide/show the host without destroying it. The hosted editor stays
    // attached. Used when the AUX tab switches away or the AUX view is
    // swapped out for MIXING / RECORDING / MASTERING.
    void setHostHidden (bool hidden);

    bool keyPressed (const juce::KeyPress& k) override;
    void resized() override;

private:
    void applyEditorLayout();

    class EditorPanel;

    juce::Component* trackedEditor = nullptr;
    juce::AudioProcessor* processor = nullptr;   // for isResizable check on the editor
    class AudioEngine* enginePtr = nullptr;
    juce::Rectangle<int> lastBounds;             // suppress redundant setBounds + Component-listener loops
    std::unique_ptr<EditorPanel> editorPanel;    // wraps the editor so the host can be larger than it (centers non-resizable plugins)
};
} // namespace duskstudio

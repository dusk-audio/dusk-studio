#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/vst3/Vst3Editor.h"

namespace duskstudio
{
namespace vst3 { class Vst3Instance; }

// JUCE bridge for a natively-hosted VST3 plugin editor: attaches to the slot's
// live Vst3Instance, creates its IPlugView, and ties the native host window to
// this Component's peer / bounds / visibility. Mirrors Lv2PluginEditorComponent;
// like the LV2 UI, the view attaches (and first exists on screen) when this
// component is actually showing.
class Vst3PluginEditorComponent final : public juce::Component,
                                         private juce::Timer
{
public:
    Vst3PluginEditorComponent();
    ~Vst3PluginEditorComponent() override;

    // Attach to an ALREADY-loaded instance owned elsewhere. We do not own its
    // lifecycle: the slot must outlive this component. False (+errorOut) when
    // the plugin ships no X11-embeddable editor.
    bool attach (vst3::Vst3Instance& shared, juce::String& errorOut);

    bool isLoaded() const noexcept { return loaded; }

    void resized() override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;
    void moved() override;

private:
    void timerCallback() override;
    void tryEmbed();
    void pushBounds();
    void verifyGeometry();
    unsigned long peerX11() const;

    vst3::Vst3Editor editor;
    double lastPumpMs = 0.0;
    bool loaded    = false;
    bool embedded  = false;
    bool embedding = false;   // guards re-entry: attached() can fire resizeView → setSize → resized()
    int  geometryCheckTick = 0;
    int  driftLogsLeft     = 10;
    bool geometryLostLogged = false;
    bool embedCheckLogged   = false;
};
} // namespace duskstudio

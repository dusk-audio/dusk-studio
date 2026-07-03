#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/lv2/Lv2Editor.h"

namespace duskstudio
{
namespace lv2 { class Lv2Instance; }

// JUCE bridge for a natively-hosted LV2 plugin UI: attaches to the slot's live
// Lv2Instance, discovers its UI through suil, and ties the native host window to
// this Component's peer / bounds / visibility. Mirrors ClapPluginEditorComponent,
// with one structural difference — an LV2 UI takes its parent at instantiate
// time, so the embed happens (and the UI first exists) when this component is
// actually on-screen.
class Lv2PluginEditorComponent final : public juce::Component,
                                        private juce::Timer
{
public:
    Lv2PluginEditorComponent();
    ~Lv2PluginEditorComponent() override;

    // Attach to an ALREADY-loaded instance owned elsewhere (NativeLv2Slot). We do
    // not own its lifecycle: the slot must outlive this component. False (+errorOut)
    // when the plugin ships no X11-embeddable UI.
    bool attach (lv2::Lv2Instance& shared, juce::String& errorOut);

    bool isLoaded() const noexcept { return loaded; }

    // App shutdown: stop pumping + leak the UI (foreign-toolkit destructors hang).
    void leakForShutdown();

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

    lv2::Lv2Editor editor;
    bool loaded    = false;
    bool embedded  = false;
    bool embedding = false;   // guards re-entry: instantiate fires ui:resize → setSize → resized()
    int  geometryCheckTick = 0;
    int  driftLogsLeft     = 10;
    bool geometryLostLogged = false;
};
} // namespace duskstudio

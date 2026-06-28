#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/clap/ClapBundle.h"
#include "../engine/clap/ClapInstance.h"
#include "../engine/clap/ClapEditor.h"

namespace duskstudio
{
// JUCE bridge for a natively-hosted CLAP plugin editor: loads a .clap, creates +
// activates the instance, opens its embedded-X11 editor through our own host, and
// ties the native host window to this Component's peer / bounds / visibility. The
// editor is embedded once (unmapped) the first time we're on a realised peer, and
// revealed (instant X map) when shown — no JUCE plugin-editor hosting involved.
//
// This is the reusable piece the aux lane will host (increment 3); for now it is
// also driven by the DUSKSTUDIO_CLAP_EDITOR_TEST launch path for live verification.
class ClapPluginEditorComponent final : public juce::Component,
                                         private juce::Timer
{
public:
    ClapPluginEditorComponent();
    ~ClapPluginEditorComponent() override;

    // Load + activate + open the editor for the plugin at `clapPath`. Sizes this
    // component to the editor's preferred size. False (+errorOut) on failure.
    bool load (const juce::File& clapPath, juce::String& errorOut);

    void resized() override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;
    void moved() override;

private:
    void timerCallback() override;
    void tryEmbed();
    void pushBounds();
    unsigned long peerX11() const;

    clap::ClapBundle   bundle;
    clap::ClapInstance instance;
    clap::ClapEditor   editor;
    bool loaded   = false;
    bool embedded = false;
    juce::uint32 lastPumpMs = 0;
};
} // namespace duskstudio

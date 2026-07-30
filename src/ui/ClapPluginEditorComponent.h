#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/clap/ClapBundle.h"
#include "../engine/clap/ClapInstance.h"
#include "../engine/clap/ClapEditor.h"
#include "../foundation/MessageThread.h"

namespace duskstudio
{
// JUCE bridge for a natively-hosted CLAP plugin editor: loads a .clap, creates +
// activates the instance, opens its native embedded editor through our own host,
// and ties the platform child container to this Component's peer, bounds, and
// visibility. No JUCE plugin-editor hosting is involved.
//
// The reusable editor piece for aux-lane hosting; also driven by the
// DUSKSTUDIO_CLAP_EDITOR_TEST launch path for live verification.
class ClapPluginEditorComponent final : public juce::Component,
                                         private dusk::Timer
{
public:
    ClapPluginEditorComponent();
    ~ClapPluginEditorComponent() override;

    // Load + activate + open the editor for the plugin at `clapPath`. Owns its own
    // instance (used by the standalone editor-test harness). False (+errorOut) on failure.
    bool load (const juce::File& clapPath, juce::String& errorOut);

    // Attach the editor to an ALREADY-loaded instance owned elsewhere (the aux lane's
    // NativeClapSlot - one instance drives audio + editor). We do not own its lifecycle:
    // the slot must outlive this component. False (+errorOut) on failure.
    bool attach (clap::ClapInstance& shared, juce::String& errorOut);

    bool isLoaded() const noexcept { return loaded; }

    // App shutdown: stop pumping + leak the plugin GUI (u-he hangs in gui->destroy).
    void leakForShutdown();

    void resized() override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;
    void moved() override;

private:
    void timerCallback() override;
    void tryEmbed();
    void pushBounds();
#if defined(__linux__)
    void verifyGeometry();
#endif
    void* peerNativeHandle() const;
    juce::Rectangle<int> editorBoundsInPeer() const;
    int componentExtentFromEditor (int extent) const;

    bool openEditorOn (clap::ClapInstance& inst, juce::String& errorOut);

    // Owned only in the load() (standalone) path; in attach() mode these stay unused
    // and the shared instance lives in the aux NativeClapSlot.
    clap::ClapBundle   bundle;
    clap::ClapInstance instance;
    clap::ClapEditor   editor;
    bool ownsInstance = false;
    bool loaded   = false;
    bool embedded = false;
    std::uint32_t lastPumpMs = 0;
#if defined(__linux__)
    int  geometryCheckTick = 0;
    int  driftLogsLeft     = 10;
    bool geometryLostLogged = false;
    bool embedCheckLogged   = false;
#endif
};
} // namespace duskstudio

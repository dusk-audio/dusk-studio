#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/clap/ClapBundle.h"
#include "../engine/clap/ClapInstance.h"
#include "../engine/clap/ClapEditor.h"
#include "../engine/clap/NativeClapSlot.h"
#include "../foundation/MessageThread.h"
#include "NativeEditorOwner.h"

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

    // Bind to the slot that owns the attached instance, so the pump tick notices
    // it being destroyed or replaced. See NativeEditorOwner.h.
    void bindOwner (clap::NativeClapSlot& slot) noexcept
    { owner.stamp (slot); ownerSlot = &slot; }

    bool ownerIsStale() const noexcept
    { return ownerSlot != nullptr && owner.isStale (*ownerSlot); }

    // Set once this editor tore itself down on one of its own guards - the reap
    // needs it because abandoning unbinds the slot.
    bool wasAbandoned() const noexcept { return abandoned; }

    // App shutdown: stop pumping + leak the plugin GUI (u-he hangs in gui->destroy).
    void leakForShutdown();

    // The instance is gone: stop pumping, drop the plugin-side handles, and
    // release what this host owns. See NativeEditorOwner.h.
    void abandonInstance();

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
    NativeEditorOwner      owner;
    clap::NativeClapSlot*  ownerSlot = nullptr;
    bool abandoned = false;
    bool ownsInstance = false;
    bool loaded    = false;
    bool embedded  = false;
    bool embedding = false;   // guards re-entry: a nested dispatch inside set_parent/show lets the timer poll re-enter tryEmbed
    std::uint32_t lastPumpMs = 0;
#if defined(__linux__)
    int  geometryCheckTick = 0;
    int  driftLogsLeft     = 10;
    bool geometryLostLogged = false;
    bool embedCheckLogged   = false;
#endif
};
} // namespace duskstudio

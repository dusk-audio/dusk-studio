#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/vst3/Vst3Editor.h"
#if DUSKSTUDIO_HAS_NATIVE_VST3
#include "../engine/vst3/NativeVst3Slot.h"
#endif
#if DUSKSTUDIO_HAS_NATIVE_AU
#include "../engine/au/AuEditor.h"
#include "../engine/au/NativeAuSlot.h"
#endif
#include "../foundation/MessageThread.h"
#include "NativeEditorOwner.h"

namespace duskstudio
{
#if DUSKSTUDIO_HAS_NATIVE_VST3
namespace vst3 { class Vst3Instance; }

// JUCE bridge for a natively-hosted VST3 plugin editor: attaches to the slot's
// live Vst3Instance, creates its IPlugView, and ties the native host window to
// this Component's peer / bounds / visibility. The view attaches (and first
// exists on screen) when this component is actually showing.
class Vst3PluginEditorComponent final : public juce::Component,
                                         private dusk::Timer
{
public:
    Vst3PluginEditorComponent();
    ~Vst3PluginEditorComponent() override;

    // Attach to an ALREADY-loaded instance owned elsewhere. We do not own its
    // lifecycle: the slot must outlive this component. False (+errorOut) when
    // the plugin ships no editor embeddable on this platform.
    bool attach (vst3::Vst3Instance& shared, juce::String& errorOut);

    bool isLoaded() const noexcept { return loaded; }

    // Bind to the slot that owns the attached instance, so the pump tick notices
    // it being destroyed or replaced. See NativeEditorOwner.h.
    void bindOwner (vst3::NativeVst3Slot& slot) noexcept
    { owner.stamp (slot); ownerSlot = &slot; }

    bool ownerIsStale() const noexcept
    { return ownerSlot != nullptr && owner.isStale (*ownerSlot); }

    // Set once this editor tore itself down on one of its own guards - the reap
    // needs it because abandoning unbinds the slot.
    bool wasAbandoned() const noexcept { return abandoned; }

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
    std::uintptr_t peerNativeHandle() const;
    juce::Rectangle<int> editorBoundsInPeer() const;
    int componentExtentFromEditor (int extent) const;

    vst3::Vst3Editor editor;
    NativeEditorOwner      owner;
    vst3::NativeVst3Slot*  ownerSlot = nullptr;
    bool abandoned = false;
    double lastPumpMs = 0.0;
    bool loaded    = false;
    bool embedded  = false;
    bool embedding = false;   // guards re-entry: attached() can fire resizeView -> setSize -> resized()
#if defined(__linux__)
    int  geometryCheckTick = 0;
    int  driftLogsLeft     = 10;
    bool geometryLostLogged = false;
    bool embedCheckLogged   = false;
#endif
};
#endif

#if DUSKSTUDIO_HAS_NATIVE_AU
namespace au { class AuInstance; }

class AuPluginEditorComponent final : public juce::Component,
                                      private dusk::Timer
{
public:
    AuPluginEditorComponent();
    ~AuPluginEditorComponent() override;

    bool attach (au::AuInstance& instance, juce::String& errorOut);
    bool isLoaded() const noexcept { return loaded; }

    // Same owner contract as Vst3PluginEditorComponent above.
    void bindOwner (au::NativeAuSlot& slot) noexcept
    { owner.stamp (slot); ownerSlot = &slot; }

    bool ownerIsStale() const noexcept
    { return ownerSlot != nullptr && owner.isStale (*ownerSlot); }

    bool wasAbandoned() const noexcept { return abandoned; }

    void abandonInstance();

    void resized() override;
    void moved() override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;

private:
    void timerCallback() override;
    void tryEmbed();
    void pushBounds();
    std::uintptr_t peerNativeHandle() const;
    juce::Rectangle<int> editorBoundsInPeer() const;

    au::AuEditor       editor;
    NativeEditorOwner  owner;
    au::NativeAuSlot*  ownerSlot = nullptr;
    bool abandoned = false;
    bool loaded = false;
    bool embedded = false;
    bool embedding = false;   // guards re-entry while embed() is adding subviews
};
#endif
} // namespace duskstudio

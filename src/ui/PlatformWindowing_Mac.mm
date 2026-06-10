#include "PlatformWindowing.h"
#include <juce_audio_processors/juce_audio_processors.h>
#import <AppKit/AppKit.h>   // NSCursor (hide/unhide) — not pulled in transitively

// Stubs for now. The Mac/Win equivalents of the Linux fixes haven't
// surfaced yet because the user smoke-tests on Linux only - but the
// fix slots need to exist so when (not if) the bugs appear, the
// scope of the change is one platform-impl file rather than a
// cross-cutting refactor. See PlatformWindowing.h for the eventual
// per-platform implementations:
//   bringWindowToFront     -> [NSApp activateIgnoringOtherApps:YES]
//                              + makeKeyAndOrderFront on the peer's
//                              NSWindow (peer.getNativeHandle() returns
//                              the NSView; .window pulls the window).
//   flushWindowOperations  -> drain a single NSRunLoop iteration with
//                              dateLimit = +1 ms.
//   prepareNativePeer...   -> no-op (NSView reparenting is
//                              synchronous, no race window).

namespace duskstudio::platform
{
namespace
{
// Wraps a parent-process juce::AudioProcessorEditor (created from
// PluginSlot's shell instance) so it can be hosted as a regular
// juce::Component inside the main app window. Owns the editor via
// unique_ptr — when the wrapper destructs, the editor destructs,
// which fires AudioProcessor::editorBeingDeleted on the shell
// instance. PluginSlot's releaseShellInstance is then safe to run
// because the editor's processor pointer no longer references the
// shell.
//
// Resize protocol:
//   • Wrapper.resized() (host changed bounds)  -> editor.setBounds
//     to fill the wrapper. Standard JUCE pattern.
//   • Editor's componentMovedOrResized (plugin-initiated scale, e.g.
//     Diva's "150%" zoom popup) -> wrapper.setSize matches the editor.
//     A parent->resized() nudge is issued so the modal container
//     re-layouts around the new size. applyingFromEditor flag breaks
//     the resize feedback loop (editor.setBounds → editor resized →
//     wrapper.setSize → wrapper.resized → editor.setBounds → ...).
class InProcessEditorHost final : public juce::Component,
                                    private juce::ComponentListener
{
public:
    explicit InProcessEditorHost (juce::AudioProcessorEditor* editor)
        : owned (editor)
    {
        jassert (owned != nullptr);
        setOpaque (true);
        setInterceptsMouseClicks (false, true);
        addAndMakeVisible (*owned);
        owned->addComponentListener (this);
        // Seed our size from the editor's current preferred size so the
        // first paint cycle isn't a 0×0 ghost.
        setSize (juce::jmax (1, owned->getWidth()),
                  juce::jmax (1, owned->getHeight()));
    }

    ~InProcessEditorHost() override
    {
        if (owned != nullptr)
            owned->removeComponentListener (this);
        // unique_ptr dtor runs after this body — destroys the editor,
        // which calls AudioProcessor::editorBeingDeleted on the shell
        // instance. Safe ordering because PluginSlot::releaseShellInstance
        // refuses while this wrapper is outstanding (tracked via
        // PluginSlot::outstandingShellWrapper SafePointer that
        // auto-nulls when we destruct).
    }

    void resized() override
    {
        if (applyingFromEditor || owned == nullptr) return;
        owned->setBounds (getLocalBounds());
    }

private:
    void componentMovedOrResized (juce::Component& c, bool wasMoved, bool wasResized) override
    {
        juce::ignoreUnused (wasMoved);
        if (! wasResized) return;
        if (&c != owned.get()) return;

        const int w = owned->getWidth();
        const int h = owned->getHeight();
        if (w == getWidth() && h == getHeight()) return;  // no-op, avoid loops

        applyingFromEditor = true;
        setSize (w, h);
        applyingFromEditor = false;

        // Nudge the parent container (typically pluginEditorModal) so
        // it re-runs its own layout against our new bounds. JUCE
        // component dirty-paint handles the redraw side; this call
        // is purely for any geometry parents drive against children.
        if (auto* parent = getParentComponent())
            parent->resized();
    }

    std::unique_ptr<juce::AudioProcessorEditor> owned;
    bool applyingFromEditor { false };
};
} // namespace


void bringWindowToFront (juce::ComponentPeer&)             {}
void flushWindowOperations()                                {}
void prepareNativePeerForChildAttach (juce::ComponentPeer&) {}

// macOS: no-op. JUCE's setMouseCursor(NoCursor) on the component under the
// overlay glyph already hides the native cursor reliably, and that's the sole
// owner of cursor visibility. Driving AppKit's process-global [NSCursor hide]/
// [unhide] counter from here too only risks unbalancing it against JUCE, so we
// stay out of it entirely. CursorOverlay still calls this, harmlessly.
void setNativeCursorVisibleOnPeer (juce::ComponentPeer&, bool /*visible*/)
{
}
void prepareForTopLevelDestruction (juce::Component& topLevel)
{
    // macOS doesn't have Mutter's focused-window-destroy assertion,
    // but defocusing before destruct is good hygiene and matches the
    // contract callsites expect.
    juce::Component::unfocusAllComponents();
    topLevel.giveAwayKeyboardFocus();
}
void clearXInputFocus() {}                 // X-only; no-op on macOS
void requestFocusOnMainWaylandSurface() {} // Wayland-only; no-op on macOS
void preferX11ForNextNativeWindow() {}     // Wayland-only; no-op on macOS
void clearPreferX11ForNativeWindow() {}    // Wayland-only; no-op on macOS
void installNonFatalXErrorHandler() {}     // X-only; no-op on macOS

std::unique_ptr<juce::Component> createForeignNativeWindowEmbed (std::uint64_t)
{
    // Cross-process NSView reparenting is its own research project and
    // we have decided not to ship it (private CARemoteLayer is the only
    // viable mechanism). Mac uses the dual-load shell pattern via
    // createInProcessEditorHost below instead; this overload stays
    // nullptr so existing callers fall through to the floating-window
    // default until the shell-editor pickup lands in sub-phase 3c-2.
    return nullptr;
}

std::unique_ptr<juce::Component> createInProcessEditorHost (
    juce::AudioProcessorEditor* shellEditor) noexcept
{
    if (shellEditor == nullptr) return nullptr;
    // The wrapper takes ownership of shellEditor via unique_ptr inside
    // its ctor. Caller (ChannelStripComponent on Mac) must NOT double-
    // own the editor; the raw pointer comes from
    // PluginSlot::createShellEditor which yields ownership intent
    // (AudioProcessor::createEditorIfNeeded contract = caller-owns).
    return std::make_unique<InProcessEditorHost> (shellEditor);
}
} // namespace duskstudio::platform

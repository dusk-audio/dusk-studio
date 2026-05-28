#include "PlatformWindowing.h"

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
void bringWindowToFront (juce::ComponentPeer&)             {}
void flushWindowOperations()                                {}
void prepareNativePeerForChildAttach (juce::ComponentPeer&) {}
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
    // Structural plumbing only (sub-phase 3c-1). Returning nullptr keeps
    // Mac OOP behaviour identical to today's main branch: the child
    // process opens its own floating native-titlebar window and the
    // parent's call site handles the nullptr by falling through to
    // that default. Sub-phase 3c-2 will:
    //   • own + size shellEditor inside a juce::Component subclass that
    //     forwards resized() to the inner editor;
    //   • inherit juce::ComponentListener so plugin-initiated size
    //     changes (Diva preset reload, dynamic editor resize) re-layout
    //     the wrapper cleanly;
    //   • coordinate destruction with PluginSlot::releaseShellInstance
    //     so the JUCE editor outlives no callbacks.
    juce::ignoreUnused (shellEditor);
    return nullptr;
}
} // namespace duskstudio::platform

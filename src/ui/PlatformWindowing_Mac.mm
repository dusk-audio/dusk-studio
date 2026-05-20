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
    // Cross-process NSView reparenting is its own research project; the
    // Mac OOP editor stays floating until that lands.
    return nullptr;
}
} // namespace duskstudio::platform

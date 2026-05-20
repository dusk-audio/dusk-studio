#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio::platform
{
// Cross-platform window-management primitives. Per-platform
// implementations live in PlatformWindowing_{Linux,Mac,Windows}.{cpp,mm}.
// Callsites stay platform-agnostic; only this header is included.
//
// Why a dedicated module rather than scattered #if guards: every
// "Linux fix" we've needed (focus-stealing-prevention defeat, X server
// flush before window destruction) has a Mac and Windows analogue
// (NSApp activate, AllowSetForegroundWindow / SetForegroundWindow on
// Win, RunLoop drain on Mac). Keeping the contract uniform makes the
// fix slot obvious when the equivalent bug surfaces on the other
// platforms - and the user develops on macOS but only smoke-tests on
// Linux, so the platform asymmetry of past bug reports does NOT mean
// the other platforms are bug-free.

// Bring the given window's native peer to the foreground and grant it
// focus. Used after creating a fresh top-level window (main window,
// plugin editor) so the WM doesn't bury it under existing windows or
// open it iconified.
//
// Linux: _NET_WM_USER_TIME = max-int + WM_CHANGE_STATE NormalState +
//        _NET_ACTIVE_WINDOW (source = pager) - the ICCCM/EWMH dance
//        that defeats Mutter's focus-stealing-prevention policy.
// macOS: [NSApp activateIgnoringOtherApps:YES] then makeKeyAndOrderFront.
// Win:   AllowSetForegroundWindow + SetForegroundWindow.
//
// Must be called on the message thread, AFTER the peer exists (i.e.
// after addToDesktop / setVisible(true)). No-op if peer is null.
void bringWindowToFront (juce::ComponentPeer& peer);

// Block until the windowing system has finished processing every
// queued operation from this process. Used to space out window-
// destruction events during shutdown so the compositor isn't asked to
// process N native-window destroys + the main window destroy back to
// back - which on Mutter+XWayland has crashed the compositor and
// taken the GNOME session with it.
//
// Linux: XSync on JUCE's display.
// macOS: drain a single NSRunLoop iteration.
// Win:   PeekMessage pump.
//
// Returns when the operation completes; deterministic, not time-based.
// Cheap on a clean compositor (microseconds).
void flushWindowOperations();

// Linux/XEmbed-only today. JUCE's VST3 editor on Linux uses
// XEmbedComponent whose host X11 window is initially parented to root
// and reparented into the host peer only when componentVisibilityChanged
// fires. Calling this before attaching a plugin editor as content of
// a freshly-mapped DocumentWindow gives the parent peer time to be
// fully realised so the reparent lands on a mapped window.
//
// Empty stub on macOS / Windows.
void prepareNativePeerForChildAttach (juce::ComponentPeer& parentPeer);

// Call BEFORE destroying any top-level juce::DocumentWindow / DialogWindow.
// On Wayland (Mutter), destroying an xdg_toplevel that the compositor
// still records as its focus_window aborts the desktop session with
// "meta_window_unmanage: focus_window != window". This helper transfers
// keyboard focus off `topLevel`'s component tree and flushes the
// windowing system so the compositor processes the focus-out event
// before the subsequent destroy lands.
//
// Cross-platform: the focus-transfer is a JUCE call (works everywhere);
// the flush is the same XSync-via-JUCE-display on Linux and a stub on
// other platforms (Mac/Windows compositors don't have the same focused-
// window-destroy assertion).
void prepareForTopLevelDestruction (juce::Component& topLevel);

// Force the X protocol's input focus to "no window" (None /
// RevertToNone) and round-trip with the server. Used as a final
// authoritative focus clear AFTER the main-window unmap and before
// the actual JUCE destroy. Plugin teardown (Diva's
// AM_VST3_Processor::terminate, etc.) can re-arm focus through
// transient helper windows we don't iterate via
// juce::TopLevelWindow; reasserting "no focus" here keeps mutter's
// focus_window NULL at the moment meta_window_unmanage runs.
//
// Linux: XSetInputFocus(None, RevertToNone) + XSync.
// Mac/Windows: no-op (the compositor / WM doesn't have the same
// focused-window-destroy assertion).
void clearXInputFocus();

// Wayland-session focus retarget. When the main window is a wl_surface
// and a plugin editor is an X11 toplevel via XWayland, mutter's
// focus_window can still be the doomed editor when its destroy lands -
// XSetInputFocus / EWMH _NET_ACTIVE_WINDOW are no-ops on Wayland sessions.
// The proper fix is xdg-activation-v1 (request the compositor activate
// the main wl_surface); the JUCE-wayland fork doesn't expose it yet, so
// the implementation today does a wl_display_roundtrip - blocks until
// mutter has dispatched its main loop, which has the side effect of
// processing the X11 unmap from XWayland and retargeting focus_window.
//
// Linux: WaylandSymbols::displayRoundtrip on JUCE's main wl_display.
// Mac/Windows: no-op.
void requestFocusOnMainWaylandSurface();

// Latch a "use X11 for top-level peer creation" flag. While set, every
// Component::createNewPeer routes to LinuxComponentPeer (X11) instead
// of WaylandComponentPeer, even on a Wayland session. Used by plugin-
// editor host wrappers because the Linux plugin protocols (VST3
// X11EmbedWindowID, LV2 LV2_UI__X11UI, JUCE-plugin X11-windowed
// renderer) need an X11 parent to attach to.
//
// Latched, not one-shot: a single juce::DocumentWindow ctor body
// triggers multiple peer recreations (TopLevelWindow base ctor,
// setUsingNativeTitleBar, setResizable, lookAndFeelChanged) - we
// need the X11 routing to hold across all of them. Caller pattern:
// preferX11ForNextNativeWindow() before construction, then
// clearPreferX11ForNativeWindow() at the end of the ctor body.
//
// Linux: sets / clears JUCE-wayland's latched skip flag.
// Mac/Windows: no-op.
void preferX11ForNextNativeWindow();
void clearPreferX11ForNativeWindow();
} // namespace duskstudio::platform

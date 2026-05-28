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
// of WaylandComponentPeer, even on a Wayland session. Used so the
// main window, popup menus, plugin editor peers, and dialog windows
// all share the X11 backend — required because the Linux plugin
// protocols (VST3 X11EmbedWindowID, LV2 LV2_UI__X11UI, JUCE-plugin
// X11-windowed renderer) need an X11 parent to attach to, AND because
// wl_surface popups can't parent to an X11 main window.
//
// Sticky on Linux: once preferX11ForNextNativeWindow() runs (during
// MainWindow ctor), the latch stays on for the process lifetime.
// clearPreferX11ForNativeWindow() is a no-op on Linux — clearing the
// latch would let the next popup menu pick wl_surface and silently
// fail to map under an X11 parent.
//
// Linux: sets JUCE-wayland's latched skip flag; clear is a no-op.
// Mac/Windows: no-op.
void preferX11ForNextNativeWindow();
void clearPreferX11ForNativeWindow();

// Factory for a Component that adopts a foreign native window handle
// (HWND on Windows, NSView on macOS, X11 Window on Linux) and embeds
// it into the JUCE component hierarchy. Used by ChannelStripComponent's
// OOP plugin-editor path to host the child process's editor window
// inside the DAW UI instead of letting it float as a separate top-level.
//
// Linux  : returns nullptr — the Linux OOP editor path already uses
//          juce::XEmbedComponent directly (better fit for the XEmbed
//          protocol than a generic foreign-window wrapper).
// Windows: returns a Component that SetParents the HWND on attach,
//          strips its top-level styles (WS_POPUP / WS_CAPTION → WS_CHILD),
//          and tracks SetWindowPos to the Component's bounds on resize.
//          On destruction, detaches the HWND back to the desktop so the
//          OOP host can cleanly destroy its window.
// macOS  : returns nullptr today — cross-process NSView reparenting is
//          its own research project; the Mac OOP editor stays floating.
std::unique_ptr<juce::Component> createForeignNativeWindowEmbed (
    std::uint64_t nativeHandle);

#if JUCE_MAC
// macOS-only: wraps a parent-process juce::AudioProcessorEditor inside
// a juce::Component. The editor's processor is the in-process "shell"
// instance owned by PluginSlot; DSP runs in the OOP child via IPC.
//
// Linux + Windows do NOT declare this overload — they reparent the
// child's native editor window into the parent process via XEmbed
// (Linux) or SetParent (Windows) and have no need for a parent-side
// shell instance. Call sites must gate on JUCE_MAC before invoking.
//
// Sub-phase 3c-1: stub returns nullptr so existing Mac OOP behaviour
// (child opens its own floating native-titlebar top-level) is
// preserved unchanged. Sub-phase 3c-2 wires the real shell-editor
// pickup + InProcessEditorHost wrapper.
//
// Threading: message thread only. The shell editor's AppKit NSView
// lives in the parent process so addAndMakeVisible / addToDesktop on
// the returned Component runs on JUCE's main MessageManager dispatch
// the same way every other parent-side editor does.
std::unique_ptr<juce::Component> createInProcessEditorHost (
    juce::AudioProcessorEditor* shellEditor) noexcept;
#endif
} // namespace duskstudio::platform

#include "PlatformWindowing.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Stubs for now. Win32 equivalents (eventual implementations):
//   bringWindowToFront     -> AllowSetForegroundWindow(GetCurrentProcessId())
//                              + SetForegroundWindow(hwnd) on the
//                              peer's HWND (peer.getNativeHandle()).
//   flushWindowOperations  -> while (PeekMessage(&msg, ...)) DispatchMessage.
//   prepareNativePeer...   -> no-op (Win32 SetParent is synchronous).

namespace duskstudio::platform
{
namespace
{
// Component that adopts a foreign HWND (cross-process, owned by the
// dusk-studio-plugin-host child) and reparents it into this JUCE
// component's peer when first added to the hierarchy. Tracks size /
// position via SetWindowPos on resized().
class ForeignHwndEmbed final : public juce::Component
{
public:
    explicit ForeignHwndEmbed (HWND foreign) : child (foreign)
    {
        setOpaque (true);
        setInterceptsMouseClicks (false, true);
    }

    ~ForeignHwndEmbed() override
    {
        if (child == nullptr) return;
        // Re-parent back to the desktop so the OOP host process can
        // cleanly destroy / hide its own window when we send HideEditor.
        // SetParent(child, nullptr) makes the window a top-level again
        // (Microsoft's docs: "A window will not be hidden after being
        // re-parented"; the OOP host will hide it via setVisible(false)
        // in handleHideEditor).
        ::SetParent (child, nullptr);
        // Restore the original top-level style so the OOP host can
        // re-show it as a stand-alone window if the user re-opens the
        // editor before the OOP host process exits.
        LONG_PTR style = ::GetWindowLongPtr (child, GWL_STYLE);
        style &= ~ (LONG_PTR) WS_CHILD;
        style |=  (LONG_PTR) WS_OVERLAPPEDWINDOW;
        ::SetWindowLongPtr (child, GWL_STYLE, style);
        ::SetWindowPos (child, nullptr, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                            | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_HIDEWINDOW);
    }

    void parentHierarchyChanged() override
    {
        if (child == nullptr || attached) return;
        auto* peer = getPeer();
        if (peer == nullptr) return;
        HWND parentHwnd = (HWND) peer->getNativeHandle();
        if (parentHwnd == nullptr) return;

        // Strip top-level chrome bits so the child draws as a borderless
        // sub-region of our component. The OOP host originally created
        // the window with WS_OVERLAPPEDWINDOW (native titlebar); we drop
        // those bits + add WS_CHILD so Windows treats the cross-process
        // HWND as a regular child window of our peer.
        LONG_PTR style = ::GetWindowLongPtr (child, GWL_STYLE);
        style &= ~ (LONG_PTR) (WS_OVERLAPPEDWINDOW | WS_POPUP | WS_CAPTION
                                | WS_SYSMENU | WS_THICKFRAME
                                | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        style |=  (LONG_PTR) (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
        ::SetWindowLongPtr (child, GWL_STYLE, style);
        ::SetParent (child, parentHwnd);
        attached = true;
        layoutChild();
    }

    void resized() override
    {
        layoutChild();
    }

private:
    void layoutChild()
    {
        if (! attached || child == nullptr) return;
        auto bounds = getBoundsInParent();   // peer-relative
        ::SetWindowPos (child, nullptr,
                          bounds.getX(), bounds.getY(),
                          bounds.getWidth(), bounds.getHeight(),
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    HWND child   { nullptr };
    bool attached { false };
};
} // namespace

void bringWindowToFront (juce::ComponentPeer&)             {}
void flushWindowOperations()                                {}
void prepareNativePeerForChildAttach (juce::ComponentPeer&) {}

// Windows: ShowCursor TRUE/FALSE is process-global and counter-based.
// Calling it repeatedly with the same target drives the internal counter
// past 0/-1, so a later opposite call no-ops and the cursor gets stuck.
// Native cursor hides reliably via JUCE's setMouseCursor(NoCursor) too, so
// this is a belt-and-braces fallback — track the applied state and only call
// ::ShowCursor on a real transition so the counter stays balanced.
void setNativeCursorVisibleOnPeer (juce::ComponentPeer&, bool visible)
{
    static bool nativeCursorVisible = true;   // message thread only
    if (visible == nativeCursorVisible) return;
    nativeCursorVisible = visible;
    ::ShowCursor (visible ? TRUE : FALSE);
}
void prepareForTopLevelDestruction (juce::Component& topLevel)
{
    // Win32 doesn't have the focused-window-destroy assertion either,
    // but defocusing before destruct is good hygiene and matches the
    // contract callsites expect.
    juce::Component::unfocusAllComponents();
    topLevel.giveAwayKeyboardFocus();
}
void clearXInputFocus() {}                 // X-only; no-op on Windows
void requestFocusOnMainWaylandSurface() {} // Wayland-only; no-op on Windows
void preferX11ForNextNativeWindow() {}     // Linux-only; no-op on Windows
void clearPreferX11ForNativeWindow() {}    // Linux-only; no-op on Windows

std::unique_ptr<juce::Component> createForeignNativeWindowEmbed (std::uint64_t nativeHandle)
{
    auto* hwnd = (HWND) (uintptr_t) nativeHandle;
    if (hwnd == nullptr || ! ::IsWindow (hwnd)) return nullptr;
    return std::make_unique<ForeignHwndEmbed> (hwnd);
}
} // namespace duskstudio::platform

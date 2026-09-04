#include "PlatformWindowing.h"
#include "NativeEditorEmbedScale.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Native counterparts to the Linux windowing operations. A peer's native
// handle is its HWND; Win32 owns restoration, activation and taskbar notice.
// The remaining operations are intentionally small platform hooks:
//   flushWindowOperations  -> while (PeekMessage(&msg, ...)) DispatchMessage.
//   prepareNativePeer...   -> no-op (Win32 SetParent is synchronous).

namespace duskstudio::platform
{
namespace
{
bool setWindowParent (HWND child, HWND parent) noexcept
{
    if (child == nullptr || ! ::IsWindow (child)) return false;
    if (parent != nullptr && ! ::IsWindow (parent)) return false;

    ::SetLastError (ERROR_SUCCESS);
    const auto previousParent = ::SetParent (child, parent);
    return previousParent != nullptr || ::GetLastError() == ERROR_SUCCESS;
}

// Component that adopts a foreign HWND (cross-process, owned by the
// dusk-studio-plugin-host child) and reparents it into this JUCE
// component's current peer. Tracks size / position via SetWindowPos on
// moves, resizes, and peer recreation.
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
        detachChild();
    }

    void parentHierarchyChanged() override
    {
        if (child == nullptr || ! ::IsWindow (child))
        {
            child = nullptr;
            attachedParent = nullptr;
            return;
        }

        auto* const peer = getPeer();
        auto* const nextParent = peer != nullptr
                                   ? static_cast<HWND> (peer->getNativeHandle())
                                   : nullptr;
        if (nextParent == attachedParent) return;
        if (nextParent == nullptr || ! ::IsWindow (nextParent))
        {
            detachChild();
            return;
        }
        // Strip top-level chrome bits so the child draws as a borderless
        // sub-region of our component. The OOP host originally created
        // the window with WS_OVERLAPPEDWINDOW (native titlebar); we drop
        // those bits + add WS_CHILD so Windows treats the cross-process
        // HWND as a regular child window of our peer.
        if (! ::IsWindow (child)) return;
        const LONG_PTR originalStyle = ::GetWindowLongPtr (child, GWL_STYLE);
        LONG_PTR style = originalStyle;
        style &= ~ (LONG_PTR) (WS_OVERLAPPEDWINDOW | WS_POPUP | WS_CAPTION
                                | WS_SYSMENU | WS_THICKFRAME
                                | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        style |= (LONG_PTR) (WS_CHILD | WS_CLIPSIBLINGS);
        if (! ::IsWindow (child)) return;
        ::SetWindowLongPtr (child, GWL_STYLE, style);
        if (! setWindowParent (child, nextParent))
        {
            if (::IsWindow (child))
                ::SetWindowLongPtr (child, GWL_STYLE, originalStyle);
            return;
        }
        attachedParent = nextParent;
        layoutChild();
        visibilityChanged();
    }

    void visibilityChanged() override
    {
        if (child == nullptr || ! ::IsWindow (child)) return;
        // Only a child we actually hold drives its own visibility. A failed
        // attach, or a detach that already put the window back on the desktop,
        // leaves it top-level and owned by the plugin host; showing it from
        // here would float the plugin's window loose over the desktop.
        if (attachedParent == nullptr || ! ::IsWindow (attachedParent)) return;
        ::ShowWindow (child, isShowing() ? SW_SHOWNA : SW_HIDE);
    }

    void resized() override
    {
        layoutChild();
    }

    void moved() override
    {
        layoutChild();
    }

private:
    void detachChild()
    {
        if (child == nullptr || ! ::IsWindow (child))
        {
            attachedParent = nullptr;
            return;
        }

        // Re-parent back to the desktop so the OOP host process can
        // cleanly destroy / hide its own window when we send HideEditor.
        // SetParent(child, nullptr) makes the window a top-level again
        // (Microsoft's docs: "A window will not be hidden after being
        // re-parented"; the HideEditor RPC handles the child side).
        if (attachedParent != nullptr && ! setWindowParent (child, nullptr)) return;
        attachedParent = nullptr;
        // Restore the original top-level style so the OOP host can
        // re-show it as a stand-alone window if the user re-opens the
        // editor before the OOP host process exits.
        if (! ::IsWindow (child)) return;
        LONG_PTR style = ::GetWindowLongPtr (child, GWL_STYLE);
        style &= ~ (LONG_PTR) WS_CHILD;
        style |=  (LONG_PTR) WS_OVERLAPPEDWINDOW;
        if (! ::IsWindow (child)) return;
        ::SetWindowLongPtr (child, GWL_STYLE, style);
        if (! ::IsWindow (child)) return;
        ::SetWindowPos (child, nullptr, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                            | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_HIDEWINDOW);
    }

    void layoutChild()
    {
        if (child == nullptr || attachedParent == nullptr
            || ! ::IsWindow (child) || ! ::IsWindow (attachedParent)) return;
        auto* const topLevel = getTopLevelComponent();
        if (topLevel == nullptr) return;
        const auto logical = topLevel->getLocalArea (this, getLocalBounds());
        const auto bounds = embedscale::toPhysical (*this, logical);
        if (! ::IsWindow (child)) return;
        ::SetWindowPos (child, nullptr,
                          bounds.getX(), bounds.getY(),
                          bounds.getWidth(), bounds.getHeight(),
                          SWP_NOZORDER | SWP_NOACTIVATE);
    }

    HWND child { nullptr };
    HWND attachedParent { nullptr };
};
} // namespace

void bringWindowToFront (juce::ComponentPeer& peer)
{
    auto* const hwnd = static_cast<HWND> (peer.getNativeHandle());
    if (hwnd == nullptr || ! ::IsWindow (hwnd)) return;

    if (::IsIconic (hwnd))
        ::ShowWindow (hwnd, SW_RESTORE);
    else if (! ::IsWindowVisible (hwnd))
        ::ShowWindow (hwnd, SW_SHOW);

    // A second-instance launch grants this process foreground rights before
    // handing over its payload. Respect any later refusal and flash the
    // taskbar instead of attaching input queues or otherwise forcing focus.
    if (::SetForegroundWindow (hwnd) == FALSE)
    {
        auto info = FLASHWINFO {
            sizeof (FLASHWINFO),
            hwnd,
            FLASHW_TRAY | FLASHW_TIMERNOFG,
            3,
            0
        };
        ::FlashWindowEx (&info);
    }
}
void flushWindowOperations()
{
    MSG message {};
    while (::PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            ::PostQuitMessage ((int) message.wParam);
            break;
        }
        ::TranslateMessage (&message);
        ::DispatchMessageW (&message);
    }
}
void prepareNativePeerForChildAttach (juce::ComponentPeer&) {}

// Windows: no-op. JUCE's setMouseCursor(NoCursor) on the component under the
// overlay glyph already hides the native cursor reliably and is the sole owner
// of cursor visibility. Win32 ::ShowCursor is a process-global counter; driving
// it from here too risks drifting out of sync with JUCE, so we stay out of it.
// CursorOverlay still calls this, harmlessly.
void setNativeCursorVisibleOnPeer (juce::ComponentPeer&, bool /*visible*/)
{
}
void prepareForTopLevelDestruction (juce::Component& topLevel)
{
    // Win32 doesn't have the focused-window-destroy assertion either,
    // but defocusing before destruct is good hygiene and matches the
    // contract callsites expect.
    juce::Component::unfocusAllComponents();
    topLevel.giveAwayKeyboardFocus();
}
bool hasUsableDisplay() { return true; }   // native windowing always present
double nativeViewBackingScale (void*) { return 1.0; }
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

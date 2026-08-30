// Opt into JUCE's exposed-X11 surface so juce::XWindowSystem (and its
// shared ::Display* connection) is reachable. The define MUST appear
// before juce_gui_basics is included, which happens transitively via
// PlatformWindowing.h - so it sits at the very top of this file.
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1

#include "PlatformWindowing.h"
#include "X11EditorTeardownError.h"

#include <X11/Xproto.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace duskstudio::platform
{
namespace
{
// JUCE owns a single ::Display* connection to the X server which we
// reuse here. Opening a fresh XOpenDisplay(nullptr) connection works
// but creates a parallel client-id whose user-time tracking is
// separate from JUCE's - some Mutter versions key focus-stealing
// prevention per connection, so a USER_TIME we set on a fresh
// connection isn't seen by Mutter's policy check against JUCE's
// connection. Reusing JUCE's display avoids that entire class of
// subtle, version-specific failure.
::Display* juceDisplay()
{
    auto* sys = juce::XWindowSystem::getInstanceWithoutCreating();
    return sys != nullptr ? sys->getDisplay() : nullptr;
}

// True when the process is attached to a Wayland session. The plugin
// editor toplevels are still X11 (via XWayland) but the main window is
// a wl_surface, which is what makes the X-side focus dance no-op.
//
// Cached on first call: $WAYLAND_DISPLAY is fixed for the lifetime of
// the session-manager-spawned process, and re-reading per call let the
// focus logic toggle if the env var were ever cleared mid-flight (which
// some shells do for child processes).
bool isWaylandSession()
{
    static const bool cached = []
    {
        const char* wd = std::getenv ("WAYLAND_DISPLAY");
        return wd != nullptr && *wd != '\0';
    }();
    return cached;
}

static_assert (BadWindow == x11::kBadWindow);
static_assert (X_ChangeWindowAttributes == x11::kChangeWindowAttributes);
static_assert (X_ReparentWindow == x11::kReparentWindow);
static_assert (X_UnmapWindow == x11::kUnmapWindow);

struct EditorTeardownErrorTrap
{
    ::Display* display = nullptr;
    std::uint64_t editorWindowId = 0;
    ::XErrorHandler previous = nullptr;
};

EditorTeardownErrorTrap* activeEditorTeardownTrap = nullptr;
std::mutex editorTeardownTrapMutex;

int editorTeardownXErrorHandler (::Display* display, ::XErrorEvent* error)
{
    auto* trap = activeEditorTeardownTrap;
    if (trap != nullptr && error != nullptr && display == trap->display
        && x11::shouldSuppressEditorTeardownError (
            error->error_code, error->request_code,
            static_cast<std::uint64_t> (error->resourceid), trap->editorWindowId))
    {
        std::fprintf (stderr,
                      "[Dusk Studio/X] ignored stale editor window 0x%lx "
                      "during teardown request %u\n",
                      static_cast<unsigned long> (error->resourceid),
                      static_cast<unsigned int> (error->request_code));
        std::fflush (stderr);
        return 0;
    }

    if (trap != nullptr && trap->previous != nullptr)
        return trap->previous (display, error);

    // A null previous handler means Xlib's fatal default was active. Preserve
    // that contract rather than silently accepting an unrelated protocol bug.
    std::fprintf (stderr,
                  "[Dusk Studio/X] unexpected X error during editor teardown "
                  "(error %u, request %u, resource 0x%lx)\n",
                  error != nullptr ? static_cast<unsigned int> (error->error_code) : 0u,
                  error != nullptr ? static_cast<unsigned int> (error->request_code) : 0u,
                  error != nullptr ? static_cast<unsigned long> (error->resourceid) : 0ul);
    std::fflush (stderr);
    std::abort();
}

juce::ComponentPeer* pickSiblingFocusTargetPeer (juce::Component& departing)
{
    auto* departingPeer = departing.getPeer();
    if (departingPeer == nullptr) return nullptr;

    const int n = juce::TopLevelWindow::getNumTopLevelWindows();
    for (int i = 0; i < n; ++i)
    {
        auto* tlw = juce::TopLevelWindow::getTopLevelWindow (i);
        if (tlw == nullptr || ! tlw->isVisible()) continue;
        auto* peer = tlw->getPeer();
        if (peer == nullptr || peer == departingPeer) continue;
        return peer;
    }
    return nullptr;
}
} // namespace

bool hasUsableDisplay()
{
    // JUCE's XWindowSystem is lazy and not yet created at preflight time,
    // so we can't reuse its ::Display*; open a throwaway connection.
    // XOpenDisplay(nullptr) reads $DISPLAY itself and returns null when no
    // X server / XWayland is reachable - the "pure Wayland, no XWayland"
    // case that otherwise crashes deep in JUCE window creation.
    if (::Display* d = ::XOpenDisplay (nullptr))
    {
        ::XCloseDisplay (d);
        return true;
    }
    return false;
}

double nativeViewBackingScale (void*) { return 1.0; }

void runX11EditorTeardown (std::uint64_t editorWindowId,
                           const std::function<void()>& teardown)
{
    if (! teardown)
        return;

    auto* display = juceDisplay();
    if (display == nullptr || editorWindowId == 0)
    {
        teardown();
        return;
    }

    const std::lock_guard<std::mutex> lock (editorTeardownTrapMutex);

    // Deliver every older error to the normal handler before narrowing the
    // policy. XEmbedComponent's destructor also syncs; the final sync covers
    // any request it queues after its internal round-trip.
    ::XSync (display, False);
    EditorTeardownErrorTrap trap { display, editorWindowId, nullptr };
    activeEditorTeardownTrap = &trap;
    trap.previous = ::XSetErrorHandler (&editorTeardownXErrorHandler);

    teardown();
    ::XSync (display, False);

    ::XSetErrorHandler (trap.previous);
    activeEditorTeardownTrap = nullptr;
}

void setNativeCursorVisibleOnPeer (juce::ComponentPeer& peer, bool visible)
{
    auto* d = juceDisplay();
    if (d == nullptr) return;
    const auto win = (::Window) (uintptr_t) peer.getNativeHandle();
    if (win == 0) return;

    // Cache one invisible 1x1 pixmap cursor for the process - JUCE's
    // setMouseCursor(NoCursor) and image-cursor paths both go through
    // routes that get dropped on this hybrid X11/Wayland setup, so we
    // do XDefineCursor directly here. XUndefineCursor restores the
    // parent window's cursor (the WM's default arrow).
    static ::Cursor invisible = 0;
    if (invisible == 0)
    {
        // Explicitly-zeroed 1x1 depth-1 bitmap: XCreatePixmap leaves contents
        // undefined, and a non-zero mask bit would render a stray cursor
        // pixel instead of nothing. An all-zero mask draws no pixels.
        static const char zeroBit = 0;
        ::Pixmap blank = ::XCreateBitmapFromData (d, win, &zeroBit, 1, 1);
        if (blank == 0) return;   // allocation failed - skip the override rather than feed a null pixmap
        ::XColor xc{};
        invisible = ::XCreatePixmapCursor (d, blank, blank, &xc, &xc, 0, 0);
        ::XFreePixmap (d, blank);
    }

    // If the cursor never got created (pixmap failure above), leave the native
    // cursor alone instead of defining None.
    if (invisible == 0) return;

    if (visible) ::XUndefineCursor (d, win);
    else         ::XDefineCursor   (d, win, invisible);
    ::XFlush (d);
}

void bringWindowToFront (juce::ComponentPeer& peer)
{
    auto* d = juceDisplay();
    if (d == nullptr) return;

    const auto win = (::Window) (uintptr_t) peer.getNativeHandle();
    const auto userTimeAtom    = ::XInternAtom (d, "_NET_WM_USER_TIME",   False);
    const auto changeStateAtom = ::XInternAtom (d, "WM_CHANGE_STATE",     False);
    const auto activeWinAtom   = ::XInternAtom (d, "_NET_ACTIVE_WINDOW",  False);

    // Max-int timestamp so user_time >= every prior user-input
    // timestamp the WM has on file. Mutter's focus-stealing-prevention
    // policy compares timestamps; a higher value reads as "this is
    // the most recent user gesture", which the policy honours.
    unsigned long t = 0x7FFFFFFFUL;
    ::XChangeProperty (d, win, userTimeAtom, XA_CARDINAL, 32,
                        PropModeReplace,
                        reinterpret_cast<unsigned char*> (&t), 1);

    // ICCCM "deminimise" request - the only standard way to ask the WM
    // to take a window out of the Iconic state from the client side.
    ::XEvent demin {};
    demin.xclient.type         = ClientMessage;
    demin.xclient.window       = win;
    demin.xclient.message_type = changeStateAtom;
    demin.xclient.format       = 32;
    demin.xclient.data.l[0]    = 1;  // NormalState
    ::XSendEvent (d, DefaultRootWindow (d), False,
                   SubstructureRedirectMask | SubstructureNotifyMask,
                   &demin);

    // EWMH activate. data.l[0] = 2 = "source: pager / taskbar" -
    // higher trust level than a regular client request, so Mutter
    // honours it even on its strictest focus policy.
    ::XEvent act {};
    act.xclient.type         = ClientMessage;
    act.xclient.window       = win;
    act.xclient.message_type = activeWinAtom;
    act.xclient.format       = 32;
    act.xclient.data.l[0]    = 2;
    act.xclient.data.l[1]    = (long) t;
    ::XSendEvent (d, DefaultRootWindow (d), False,
                   SubstructureRedirectMask | SubstructureNotifyMask,
                   &act);

    // EWMH activate marks the window as the X focus target, but Mutter's XWayland
    // bridge doesn't actually route keyboard input to it until the user clicks -
    // so every widget needs two clicks (first focuses, second hits). Force input
    // focus directly. Guard on viewability: XSetInputFocus on an unmapped window
    // is a BadMatch.
    ::XWindowAttributes attrs {};
    if (::XGetWindowAttributes (d, win, &attrs) != 0 && attrs.map_state == IsViewable)
        ::XSetInputFocus (d, win, RevertToParent, CurrentTime);

    ::XFlush (d);
}

void flushWindowOperations()
{
    if (auto* d = juceDisplay())
        ::XSync (d, False);
}

void prepareNativePeerForChildAttach (juce::ComponentPeer&)
{
    // Currently no Linux-specific prep needed beyond what JUCE does
    // internally - left as a hook for the XEmbed-timing fix in
    // ChannelStripComponent::PluginEditorWindow if we want to
    // consolidate it here later.
}

void prepareForTopLevelDestruction (juce::Component& topLevel)
{
    juce::Component::unfocusAllComponents();
    topLevel.giveAwayKeyboardFocus();

    auto* d = juceDisplay();

    if (isWaylandSession())
    {
        // Wayland-session path: the doomed plugin editor is an X11
        // toplevel (via XWayland), the main Dusk Studio window is a
        // wl_surface. Mutter does NOT honour X11 _NET_ACTIVE_WINDOW /
        // XSetInputFocus / XIconify for focus_window updates on a
        // Wayland session - the EWMH dance below is therefore a
        // no-op. What we CAN do is unmap the doomed window cleanly
        // and then yield to the compositor so it dispatches the
        // resulting events on its main loop - which retargets
        // focus_window off the unmapped X11 window.
        if (d != nullptr)
        {
            if (auto* peer = topLevel.getPeer())
            {
                const auto win = (::Window) (uintptr_t) peer->getNativeHandle();
                ::XWithdrawWindow (d, win, DefaultScreen (d));
            }
            ::XSync (d, False);
            std::fprintf (stderr, "[Dusk Studio/Wayland] X11 unmap + roundtrip\n");
        }
        requestFocusOnMainWaylandSurface();
        std::fflush (stderr);
        return;
    }

    // Xorg session: the doomed window AND any sibling are both real
    // X11 toplevels. EWMH _NET_ACTIVE_WINDOW on a sibling is the only
    // path mutter actually honours for X11 focus_window retargeting;
    // when no sibling exists, fall back to XIconify which routes
    // through mutter's WM_CHANGE_STATE handler.
    if (auto* sibling = pickSiblingFocusTargetPeer (topLevel))
    {
        bringWindowToFront (*sibling);
        std::fprintf (stderr, "[Dusk Studio/X] focus -> sibling peer (EWMH)\n");
    }
    else if (d != nullptr)
    {
        if (auto* peer = topLevel.getPeer())
        {
            const auto win = (::Window) (uintptr_t) peer->getNativeHandle();
            ::XIconifyWindow (d, win, DefaultScreen (d));
            std::fprintf (stderr, "[Dusk Studio/X] focus -> iconify (no sibling)\n");
        }
        else
        {
            std::fprintf (stderr, "[Dusk Studio/X] focus -> none (no peer)\n");
        }
    }
    else
    {
        std::fprintf (stderr, "[Dusk Studio/X] focus -> none (no display)\n");
    }
    std::fflush (stderr);

    if (d != nullptr)
        ::XSync (d, False);

    flushWindowOperations();
}

void clearXInputFocus()
{
    if (auto* d = juceDisplay())
    {
        ::XSetInputFocus (d, None, RevertToNone, CurrentTime);
        ::XSync (d, False);
    }
}

// All three helpers below touch the plugdata-team JUCE-wayland fork's
// WaylandWindowSystem / WaylandSymbols. Upstream JUCE has no such
// types. DUSKSTUDIO_JUCE_HAS_WAYLAND is set by CMake when the fork is in
// use; absent that, these are stubs (CI builds + non-Wayland deploys
// don't need the X11/Wayland peer-creation latch or the focus
// roundtrip).
void preferX11ForNextNativeWindow()
{
   #if DUSKSTUDIO_JUCE_HAS_WAYLAND
    juce::WaylandWindowSystem::setSkipForPeerCreation (true);
   #endif
}

void clearPreferX11ForNativeWindow()
{
    // No-op on Linux. Dusk Studio routes ALL native peers to X11
    // (XWayland) so the main window, popup menus, plugin editors,
    // and dialog windows share one peer backend. Clearing the latch
    // would let the next PopupMenu create a wl_surface peer that can't
    // parent to the X11 main window - symptom: picker menu never opens.
    // Once preferX11ForNextNativeWindow() is called (during MainWindow
    // ctor), the latch stays on for the process lifetime.
}

void requestFocusOnMainWaylandSurface()
{
   #if DUSKSTUDIO_JUCE_HAS_WAYLAND
    // The plumbed-in JUCE-wayland fork doesn't expose xdg-activation-v1
    // (no protocol XML codegen, no registry binding, no public API);
    // adding it cleanly is a separate fork-side change. Until then we
    // yield by roundtripping the Wayland connection - the compositor
    // responds only after its main loop has dispatched its queue,
    // which on a Wayland session includes processing any X11 unmaps
    // that arrived from XWayland. Mutter's focus_window for the
    // doomed X11 toplevel is therefore retargeted off it before this
    // call returns. Pair with a callAsync at the call site so the
    // subsequent X11 destroy lands an additional message-loop tick
    // later, providing extra slack for any compositor work that
    // didn't make it into the same dispatch round.
    auto* sys = juce::WaylandWindowSystem::getInstanceWithoutCreating();
    if (sys == nullptr) return;
    auto* display = sys->getDisplay();
    if (display == nullptr) return;
    juce::WaylandSymbols::getInstance()->displayRoundtrip (display);
   #endif
}

std::unique_ptr<juce::Component> createForeignNativeWindowEmbed (std::uint64_t)
{
    // Linux uses juce::XEmbedComponent directly in ChannelStripComponent;
    // no generic-wrapper path needed here.
    return nullptr;
}
} // namespace duskstudio::platform

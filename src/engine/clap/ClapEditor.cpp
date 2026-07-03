#include "ClapEditor.h"

#include <algorithm>

#include <X11/Xlib.h>

namespace duskstudio::clap
{
ClapEditor::~ClapEditor() { close(); }

bool ClapEditor::open (const ::clap_plugin* p, ClapHost& host, std::string& errorOut)
{
    if (p == nullptr) { errorOut = "null plugin"; return false; }

    gui = static_cast<const clap_plugin_gui_t*> (p->get_extension (p, CLAP_EXT_GUI));
    if (gui == nullptr) { errorOut = "plugin has no gui extension"; return false; }
    if (gui->is_api_supported == nullptr
        || ! gui->is_api_supported (p, CLAP_WINDOW_API_X11, false))
    { errorOut = "plugin has no embedded-X11 GUI"; gui = nullptr; return false; }
    if (gui->create == nullptr || ! gui->create (p, CLAP_WINDOW_API_X11, false))
    { errorOut = "gui create() failed"; gui = nullptr; return false; }

    plugin  = p;
    hostPtr = &host;
    host.setPlugin (p);
    host.setCallbacks (this);
    created = true;

    // A non-resizable plugin (u-he Satin, etc.) ABORTS if the host calls set_size on
    // it — so we must only ever set_size when this is true, and otherwise size our host
    // window to the plugin's own preferred size.
    resizable = (gui->can_resize != nullptr) && gui->can_resize (p);

    uint32_t w = 0, h = 0;
    if (gui->get_size != nullptr && gui->get_size (p, &w, &h))
    { prefW = (int) w; prefH = (int) h; }
    return true;
}

bool ClapEditor::embed (unsigned long parentX11, int x, int y, int w, int h, std::string& errorOut)
{
    if (! created) { errorOut = "gui not created"; return false; }

    auto* dpy = XOpenDisplay (nullptr);
    if (dpy == nullptr) { errorOut = "XOpenDisplay failed"; return false; }
    display = dpy;

    const int ww = w > 0 ? w : (prefW > 0 ? prefW : 400);
    const int hh = h > 0 ? h : (prefH > 0 ? prefH : 300);

    // Solid (black) background, NOT background_pixmap=None: on map the server fills
    // the window with this pixel instead of leaving stale framebuffer behind it
    // (otherwise the mixer underneath shows through until the plugin's child paints).
    XSetWindowAttributes swa {};
    swa.background_pixel = BlackPixel (dpy, DefaultScreen (dpy));
    swa.border_pixel     = 0;
    swa.event_mask       = StructureNotifyMask;
    hostWindow = XCreateWindow (dpy, (Window) parentX11, x, y,
                                (unsigned) ww, (unsigned) hh, 0,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWBackPixel | CWBorderPixel | CWEventMask, &swa);
    // Map the host window BEFORE set_parent: some plugins (u-he Satin) abort() when
    // reparented into a non-viewable window. tryEmbed only runs when this component is
    // on-screen, so showing the host now is correct — it sits at the lane's coords with
    // a solid bg until the plugin paints (no stale content, no stray-window flash).
    XMapWindow (dpy, hostWindow);
    XSync (dpy, False);

    clap_window_t win {};
    win.api = CLAP_WINDOW_API_X11;
    win.x11 = (clap_xwnd) hostWindow;
    if (gui->set_parent == nullptr || ! gui->set_parent (plugin, &win))
    { errorOut = "gui set_parent() failed"; close(); return false; }

    if (resizable && gui->set_size != nullptr) gui->set_size (plugin, (uint32_t) ww, (uint32_t) hh);
    if (gui->show != nullptr && ! gui->show (plugin))
    { errorOut = "gui show() failed"; close(); return false; }

    embedded = true;
    mapped   = true;
    return true;
}

void ClapEditor::setBounds (int x, int y, int w, int h)
{
    if (display == nullptr || hostWindow == 0) return;
    auto* dpy = (Display*) display;
    XMoveResizeWindow (dpy, hostWindow, x, y,
                       (unsigned) std::max (1, w), (unsigned) std::max (1, h));
    if (resizable && gui != nullptr && gui->set_size != nullptr && w > 0 && h > 0)
        gui->set_size (plugin, (uint32_t) w, (uint32_t) h);
    XFlush (dpy);
}

bool ClapEditor::getActualGeometry (int& x, int& y, int& w, int& h) const
{
    if (! embedded || display == nullptr || hostWindow == 0)
        return false;
    ::Window root {};
    unsigned int uw = 0, uh = 0, border = 0, depth = 0;
    if (XGetGeometry ((Display*) display, (Window) hostWindow,
                      &root, &x, &y, &uw, &uh, &border, &depth) == 0)
        return false;
    w = (int) uw; h = (int) uh;
    return true;
}

void ClapEditor::reveal()
{
    if (display == nullptr || hostWindow == 0 || mapped) return;
    XMapWindow ((Display*) display, hostWindow);
    XFlush ((Display*) display);
    mapped = true;
}

void ClapEditor::hide()
{
    if (display == nullptr || hostWindow == 0 || ! mapped) return;
    XUnmapWindow ((Display*) display, hostWindow);
    XFlush ((Display*) display);
    mapped = false;
}

void ClapEditor::close()
{
    // On shutdown, leak the plugin GUI — u-he's gui->destroy hangs. Otherwise tear
    // it down normally.
    if (plugin != nullptr && gui != nullptr && ! leakOnClose)
    {
        if (gui->hide != nullptr)    gui->hide (plugin);
        if (gui->destroy != nullptr) gui->destroy (plugin);
    }
    if (hostPtr != nullptr) { hostPtr->setCallbacks (nullptr); hostPtr = nullptr; }
    if (display != nullptr)
    {
        if (hostWindow != 0) XDestroyWindow ((Display*) display, hostWindow);
        // Let the server process the destroy (against a still-valid parent peer)
        // before we drop this connection — avoids a cross-connection teardown hang.
        XSync ((Display*) display, False);
        XCloseDisplay ((Display*) display);
        display = nullptr;
    }
    hostWindow = 0;
    gui = nullptr;
    plugin = nullptr;
    created = embedded = mapped = false;
}

void ClapEditor::drainPendingCallbacks()
{
    // Message thread. Process a destroy first so a coalesced resize/show after it
    // can't drive a torn-down GUI.
    if (pendingClosed.exchange (false))
    {
        if (pendingClosedWasDestroyed.load())
        {
            // CLAP gui.h: on was_destroyed the host MUST call destroy() to ack.
            if (plugin != nullptr && gui != nullptr && gui->destroy != nullptr)
                gui->destroy (plugin);
            gui = nullptr;            // the GUI is gone — stop treating it as live
            created = embedded = false;
            // The plugin tore down its own GUI; unmap our now-empty host window so it
            // doesn't linger as a black rectangle until close(). The display + window
            // are fully released in close().
            hide();
        }
        if (onClosed) onClosed();
        // The GUI is gone — drop any coalesced resize/show/hide so a queued show
        // can't remap (or a resize can't poke) the now-empty host window.
        pendingResize.exchange (false);
        pendingShow.exchange (false);
        pendingHide.exchange (false);
        return;
    }

    if (pendingResize.exchange (false))
    {
        const int w = pendingW.load (std::memory_order_relaxed);
        const int h = pendingH.load (std::memory_order_relaxed);
        prefW = w; prefH = h;
        if (display != nullptr && hostWindow != 0)
            XResizeWindow ((Display*) display, hostWindow,
                           (unsigned) std::max (1, w), (unsigned) std::max (1, h));
        if (onResize) onResize (w, h);
    }

    if (pendingShow.exchange (false)) reveal();
    if (pendingHide.exchange (false)) hide();
}

void ClapEditor::pump (double elapsedMs)
{
    drainPendingCallbacks();

    if (hostPtr != nullptr) hostPtr->pumpGui (elapsedMs);

    // Drain our host-window connection so its event queue (ConfigureNotify, etc.)
    // doesn't grow unbounded. The plugin's own window events ride its own fd,
    // pumped above via on_fd.
    if (display != nullptr)
    {
        auto* dpy = (Display*) display;
        while (XPending (dpy) > 0)
        {
            XEvent e;
            XNextEvent (dpy, &e);
        }
    }
}

// These four may be called from the plugin's own thread (CLAP marks them
// [thread-safe]). They only record intent; drainPendingCallbacks() applies it on the
// message thread. request_resize returning true means "acknowledged, applied async".
bool ClapEditor::onRequestResize (uint32_t w, uint32_t h)
{
    pendingW.store ((int) w, std::memory_order_relaxed);
    pendingH.store ((int) h, std::memory_order_relaxed);
    pendingResize.store (true, std::memory_order_release);
    return true;
}

bool ClapEditor::onRequestShow() { pendingShow.store (true, std::memory_order_release); return true; }
bool ClapEditor::onRequestHide() { pendingHide.store (true, std::memory_order_release); return true; }

void ClapEditor::onGuiClosed (bool wasDestroyed)
{
    pendingClosedWasDestroyed.store (wasDestroyed, std::memory_order_relaxed);
    pendingClosed.store (true, std::memory_order_release);
}
} // namespace duskstudio::clap

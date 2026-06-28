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

    XSetWindowAttributes swa {};
    swa.background_pixmap = None;
    swa.border_pixel      = 0;
    swa.event_mask        = StructureNotifyMask;
    hostWindow = XCreateWindow (dpy, (Window) parentX11, x, y,
                                (unsigned) ww, (unsigned) hh, 0,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWBackPixmap | CWBorderPixel | CWEventMask, &swa);
    // Deliberately NOT mapped: born as a child of the app window but hidden until
    // reveal(), so the plugin can embed + render with no stray-window flash.
    XSync (dpy, False);

    clap_window_t win {};
    win.api = CLAP_WINDOW_API_X11;
    win.x11 = (clap_xwnd) hostWindow;
    if (gui->set_parent == nullptr || ! gui->set_parent (plugin, &win))
    { errorOut = "gui set_parent() failed"; close(); return false; }

    if (gui->set_size != nullptr) gui->set_size (plugin, (uint32_t) ww, (uint32_t) hh);
    // show() builds + maps the plugin's own window as a child of our (still
    // unmapped) host — invisible until reveal() maps the host.
    if (gui->show != nullptr) gui->show (plugin);

    embedded = true;
    return true;
}

void ClapEditor::setBounds (int x, int y, int w, int h)
{
    if (display == nullptr || hostWindow == 0) return;
    auto* dpy = (Display*) display;
    XMoveResizeWindow (dpy, hostWindow, x, y,
                       (unsigned) std::max (1, w), (unsigned) std::max (1, h));
    if (gui != nullptr && gui->set_size != nullptr && w > 0 && h > 0)
        gui->set_size (plugin, (uint32_t) w, (uint32_t) h);
    XFlush (dpy);
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
    if (plugin != nullptr && gui != nullptr)
    {
        if (gui->hide != nullptr)    gui->hide (plugin);
        if (gui->destroy != nullptr) gui->destroy (plugin);
    }
    if (hostPtr != nullptr) { hostPtr->setCallbacks (nullptr); hostPtr = nullptr; }
    if (display != nullptr)
    {
        if (hostWindow != 0) XDestroyWindow ((Display*) display, hostWindow);
        XCloseDisplay ((Display*) display);
        display = nullptr;
    }
    hostWindow = 0;
    gui = nullptr;
    plugin = nullptr;
    created = embedded = mapped = false;
}

void ClapEditor::pump (double elapsedMs)
{
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

bool ClapEditor::onRequestResize (uint32_t w, uint32_t h)
{
    prefW = (int) w;
    prefH = (int) h;
    if (display != nullptr && hostWindow != 0)
        XResizeWindow ((Display*) display, hostWindow, w, h);
    if (onResize) onResize ((int) w, (int) h);
    return true;
}

bool ClapEditor::onRequestShow() { reveal(); return true; }
bool ClapEditor::onRequestHide() { hide();   return true; }
void ClapEditor::onGuiClosed (bool) { if (onClosed) onClosed(); }
} // namespace duskstudio::clap

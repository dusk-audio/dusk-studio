#include "Vst3Editor.h"
#include "Vst3HostContext.h"
#include "Vst3Instance.h"

#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <X11/Xlib.h>

#include <algorithm>

namespace duskstudio::vst3
{
using namespace Steinberg;

struct Vst3Editor::Impl
{
    Vst3Editor*      owner    = nullptr;
    Vst3Instance*    instance = nullptr;
    Vst3HostContext* host     = nullptr;

    IPtr<IPlugView> view;

    void*         display    = nullptr;   // Display*
    unsigned long hostWindow = 0;

    int   prefW = 0, prefH = 0;
    int   lastW = 0, lastH = 0;   // last size confirmed to the view via onSize
    float contentScale = 0.0f;
    bool  embedded = false, mapped = false;

    // ── IPlugFrame::resizeView, routed through the instance's resize handler ──
    bool onResizeView (int w, int h)
    {
        if (w <= 0 || h <= 0 || ! view) return false;
        prefW = w; prefH = h;
        if (display != nullptr && hostWindow != 0)
        {
            XResizeWindow ((Display*) display, (Window) hostWindow, (unsigned) w, (unsigned) h);
            XFlush ((Display*) display);
        }
        if (owner->onResize) owner->onResize (w, h);
        // Spec: a honoured resizeView is confirmed back with onSize. The owner's
        // setSize chain may already have pushed it — lastW/lastH dedups.
        confirmSize (w, h);
        return true;
    }

    void confirmSize (int w, int h)
    {
        if (! view || (w == lastW && h == lastH)) return;
        lastW = w; lastH = h;
        ViewRect r (0, 0, w, h);
        view->onSize (&r);
    }

    void applyContentScale()
    {
        if (! view || contentScale <= 0.0f) return;
        FUnknownPtr<IPlugViewContentScaleSupport> scale (view);
        if (scale)
            scale->setContentScaleFactor (contentScale);
    }
};

Vst3Editor::Vst3Editor() : impl (std::make_unique<Impl>()) { impl->owner = this; }
Vst3Editor::~Vst3Editor() { close(); }

int  Vst3Editor::preferredWidth()  const noexcept { return impl->prefW; }
int  Vst3Editor::preferredHeight() const noexcept { return impl->prefH; }
bool Vst3Editor::isOpen()     const noexcept { return impl->view != nullptr; }
bool Vst3Editor::isEmbedded() const noexcept { return impl->embedded; }

bool Vst3Editor::open (Vst3Instance& inst, std::string& errorOut)
{
    if (impl->view) return true;

    auto* controller = static_cast<Vst::IEditController*> (inst.editController());
    if (controller == nullptr) { errorOut = "plugin has no edit controller"; return false; }

    impl->view = owned (controller->createView (Vst::ViewType::kEditor));
    if (! impl->view) { errorOut = "plugin has no editor view"; return false; }

    if (impl->view->isPlatformTypeSupported (kPlatformTypeX11EmbedWindowID) != kResultTrue)
    { errorOut = "editor does not support X11 embedding"; impl->view = nullptr; return false; }

    impl->instance = &inst;
    impl->host = &inst.getHost();
    inst.setResizeViewHandler ([self = impl.get()] (int w, int h)
                               { return self->onResizeView (w, h); });
    impl->view->setFrame (static_cast<IPlugFrame*> (impl->host->plugFrame()));

    ViewRect size {};
    if (impl->view->getSize (&size) == kResultOk)
    { impl->prefW = size.getWidth(); impl->prefH = size.getHeight(); }
    return true;
}

bool Vst3Editor::embed (unsigned long parentX11, int x, int y, int w, int h, std::string& errorOut)
{
    if (! impl->view) { errorOut = "no view open"; return false; }
    if (impl->embedded) return true;

    auto* dpy = XOpenDisplay (nullptr);
    if (dpy == nullptr) { errorOut = "XOpenDisplay failed"; return false; }
    impl->display = dpy;

    const int ww = w > 0 ? w : (impl->prefW > 0 ? impl->prefW : 480);
    const int hh = h > 0 ? h : (impl->prefH > 0 ? impl->prefH : 320);

    // Solid background (not None) so the map fills with black instead of stale
    // framebuffer — same rationale as the CLAP/LV2 editors.
    XSetWindowAttributes swa {};
    swa.background_pixel = BlackPixel (dpy, DefaultScreen (dpy));
    swa.border_pixel     = 0;
    swa.event_mask       = StructureNotifyMask;
    // Keep the WM's hands off this window in every transient state (JUCE's
    // XEmbed host sets the same flag on its host window): under mutter's
    // x11-frames a managed adoption detaches the compositor-side surface
    // from the X-space parent-child position.
    swa.override_redirect = True;
    impl->hostWindow = XCreateWindow (dpy, (Window) parentX11, x, y,
                                      (unsigned) ww, (unsigned) hh, 0,
                                      CopyFromParent, InputOutput, (Visual*) CopyFromParent,
                                      CWBackPixel | CWBorderPixel | CWEventMask | CWOverrideRedirect, &swa);
    // Viewable BEFORE the view attaches — toolkit-backed editors can abort
    // realising into an unmapped parent.
    XMapWindow (dpy, impl->hostWindow);
    XSync (dpy, False);

    impl->applyContentScale();

    if (impl->view->attached ((void*) (uintptr_t) impl->hostWindow,
                              kPlatformTypeX11EmbedWindowID) != kResultOk)
    { errorOut = "IPlugView::attached failed"; close(); return false; }

    // attached() may have re-decided the size (resizeView fires synchronously
    // through the frame) — re-query so the owner adopts the real one.
    ViewRect size {};
    if (impl->view->getSize (&size) == kResultOk
        && size.getWidth() > 0 && size.getHeight() > 0)
    { impl->prefW = size.getWidth(); impl->prefH = size.getHeight(); }

    impl->embedded = true;
    impl->mapped   = true;
    return true;
}

void Vst3Editor::setBounds (int x, int y, int w, int h)
{
    if (impl->display == nullptr || impl->hostWindow == 0) return;
    const int ww = std::max (1, w), hh = std::max (1, h);
    XMoveResizeWindow ((Display*) impl->display, (Window) impl->hostWindow,
                       x, y, (unsigned) ww, (unsigned) hh);
    XFlush ((Display*) impl->display);
    if (impl->embedded)
        impl->confirmSize (ww, hh);
}

void Vst3Editor::setContentScale (float scale)
{
    impl->contentScale = scale;
    impl->applyContentScale();
}

bool Vst3Editor::getRootRelativePosition (unsigned long referenceWindow,
                                           int& relX, int& relY) const
{
    if (! impl->embedded || impl->display == nullptr || impl->hostWindow == 0)
        return false;
    auto* dpy = (Display*) impl->display;
    ::Window dummy {};
    int hx = 0, hy = 0, rx = 0, ry = 0;
    if (! XTranslateCoordinates (dpy, (Window) impl->hostWindow,
                                 DefaultRootWindow (dpy), 0, 0, &hx, &hy, &dummy)
        || ! XTranslateCoordinates (dpy, (Window) referenceWindow,
                                    DefaultRootWindow (dpy), 0, 0, &rx, &ry, &dummy))
        return false;
    relX = hx - rx; relY = hy - ry;
    return true;
}

bool Vst3Editor::getActualGeometry (int& x, int& y, int& w, int& h) const
{
    if (! impl->embedded || impl->display == nullptr || impl->hostWindow == 0)
        return false;
    ::Window root {};
    unsigned int uw = 0, uh = 0, border = 0, depth = 0;
    if (XGetGeometry ((Display*) impl->display, (Window) impl->hostWindow,
                      &root, &x, &y, &uw, &uh, &border, &depth) == 0)
        return false;
    w = (int) uw; h = (int) uh;
    return true;
}

void Vst3Editor::reveal()
{
    if (impl->display == nullptr || impl->hostWindow == 0 || impl->mapped) return;
    XMapWindow ((Display*) impl->display, (Window) impl->hostWindow);
    XFlush ((Display*) impl->display);
    impl->mapped = true;
}

void Vst3Editor::hide()
{
    if (impl->display == nullptr || impl->hostWindow == 0 || ! impl->mapped) return;
    XUnmapWindow ((Display*) impl->display, (Window) impl->hostWindow);
    XFlush ((Display*) impl->display);
    impl->mapped = false;
}

void Vst3Editor::close()
{
    if (impl->view)
    {
        if (impl->embedded)
            impl->view->removed();
        impl->view->setFrame (nullptr);
        impl->view = nullptr;
    }
    if (impl->instance != nullptr)
    {
        impl->instance->setResizeViewHandler (nullptr);
        impl->instance = nullptr;
    }
    impl->host = nullptr;

    if (impl->display != nullptr)
    {
        if (impl->hostWindow != 0)
            XDestroyWindow ((Display*) impl->display, (Window) impl->hostWindow);
        // Flush the destroy against a still-valid parent peer before dropping
        // the connection (see ClapEditor::close).
        XSync ((Display*) impl->display, False);
        XCloseDisplay ((Display*) impl->display);
        impl->display = nullptr;
    }
    impl->hostWindow = 0;
    impl->embedded = impl->mapped = false;
    impl->prefW = impl->prefH = impl->lastW = impl->lastH = 0;
}

void Vst3Editor::pump (double elapsedMs)
{
    if (impl->host != nullptr)
        impl->host->pump (elapsedMs);

    if (impl->display != nullptr)
    {
        auto* dpy = (Display*) impl->display;
        while (XPending (dpy) > 0)
        {
            XEvent e;
            XNextEvent (dpy, &e);
        }
    }
}
} // namespace duskstudio::vst3

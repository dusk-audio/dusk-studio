#include "Vst3Editor.h"
#include "Vst3HostContext.h"
#include "Vst3Instance.h"

#import <AppKit/AppKit.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow-field-in-constructor"
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"

#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#pragma clang diagnostic pop

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
    NSView* container = nil;

    int  prefW = 0, prefH = 0;
    int  lastW = 0, lastH = 0;
    bool embedded = false, visible = false;

    void confirmSize (int w, int h)
    {
        if (! view || (w == lastW && h == lastH)) return;
        lastW = w;
        lastH = h;
        ViewRect size (0, 0, w, h);
        view->onSize (&size);
    }

    bool onResizeView (int w, int h)
    {
        if (w <= 0 || h <= 0 || ! view) return false;
        prefW = w;
        prefH = h;

        if (container != nil)
        {
            auto frame = [container frame];
            frame.size = NSMakeSize ((CGFloat) w, (CGFloat) h);
            [container setFrame:frame];
        }

        if (owner->onResize)
            owner->onResize (w, h);

        confirmSize (w, h);
        return true;
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

    if (impl->view->isPlatformTypeSupported (kPlatformTypeNSView) != kResultTrue)
    {
        errorOut = "editor does not support Cocoa embedding";
        impl->view = nullptr;
        return false;
    }

    impl->instance = &inst;
    impl->host = &inst.getHost();
    inst.setResizeViewHandler ([self = impl.get()] (int w, int h)
                               { return self->onResizeView (w, h); });
    impl->view->setFrame (static_cast<IPlugFrame*> (impl->host->plugFrame()));

    ViewRect size {};
    if (impl->view->getSize (&size) == kResultOk)
    {
        impl->prefW = size.getWidth();
        impl->prefH = size.getHeight();
    }
    return true;
}

bool Vst3Editor::embed (std::uintptr_t parentHandle, int x, int y, int w, int h,
                        std::string& errorOut)
{
    if (! impl->view) { errorOut = "no view open"; return false; }
    if (impl->embedded) return true;
    if (parentHandle == 0) { errorOut = "null parent view"; return false; }

    auto* parent = reinterpret_cast<NSView*> (parentHandle);
    const int ww = w > 0 ? w : (impl->prefW > 0 ? impl->prefW : 480);
    const int hh = h > 0 ? h : (impl->prefH > 0 ? impl->prefH : 320);

    impl->container = [[NSView alloc] initWithFrame:NSMakeRect (
        (CGFloat) x, (CGFloat) y, (CGFloat) ww, (CGFloat) hh)];
    if (impl->container == nil)
    {
        errorOut = "could not create Cocoa container";
        return false;
    }
    [impl->container setAutoresizingMask:NSViewNotSizable];
    [impl->container setHidden:YES];
    [parent addSubview:impl->container];

    if (impl->view->attached (impl->container, kPlatformTypeNSView) != kResultOk)
    {
        errorOut = "IPlugView::attached failed";
        close();
        return false;
    }

    ViewRect size {};
    if (impl->view->getSize (&size) == kResultOk
        && size.getWidth() > 0 && size.getHeight() > 0)
    {
        impl->prefW = size.getWidth();
        impl->prefH = size.getHeight();
    }

    impl->embedded = true;
    return true;
}

void Vst3Editor::setBounds (int x, int y, int w, int h)
{
    if (impl->container == nil) return;
    const int ww = std::max (1, w);
    const int hh = std::max (1, h);
    [impl->container setFrame:NSMakeRect (
        (CGFloat) x, (CGFloat) y, (CGFloat) ww, (CGFloat) hh)];
    if (impl->embedded)
        impl->confirmSize (ww, hh);
}

// Cocoa's IPlugView coordinates are already logical; the backing NSView tracks
// the screen scale without IPlugViewContentScaleSupport.
void Vst3Editor::setContentScale (float) {}

void Vst3Editor::reveal()
{
    if (impl->container == nil || impl->visible) return;
    [impl->container setHidden:NO];
    impl->visible = true;
}

void Vst3Editor::hide()
{
    if (impl->container == nil || ! impl->visible) return;
    [impl->container setHidden:YES];
    impl->visible = false;
}

void Vst3Editor::abandonPlugin() noexcept
{
    (void) impl->view.take();
    impl->instance = nullptr;
    impl->host     = nullptr;
    impl->embedded = false;
}

void Vst3Editor::abandonPluginAndContainer() noexcept
{
    abandonPlugin();
    // Hide rather than detach: attached() made the plugin's view a subview and
    // it must not leave the window.
    @try { if (impl->visible) [impl->container setHidden:YES]; }
    @catch (NSException*) {}
    impl->container = nil;
    impl->visible   = false;
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

    if (impl->container != nil)
    {
        [impl->container removeFromSuperview];
        [impl->container release];
        impl->container = nil;
    }

    impl->embedded = impl->visible = false;
    impl->prefW = impl->prefH = impl->lastW = impl->lastH = 0;
}

bool Vst3Editor::getRootRelativePosition (std::uintptr_t, int&, int&) const { return false; }
bool Vst3Editor::getActualGeometry (int&, int&, int&, int&) const { return false; }

void Vst3Editor::pump (double elapsedMs)
{
    if (impl->host != nullptr)
        impl->host->pump (elapsedMs);
}
} // namespace duskstudio::vst3

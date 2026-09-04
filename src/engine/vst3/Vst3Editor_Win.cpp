// Ahead of every include: whichever header reaches windows.h first must see
// these, or its min/max macros eat std::max at the call sites below.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "Vst3Editor.h"
#include "Vst3HostContext.h"
#include "Vst3Instance.h"

#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <algorithm>

namespace duskstudio::vst3
{
using namespace Steinberg;

namespace
{
// One process-wide class for every VST3 container window. The plugin attaches
// its own HWND inside ours, so our window procedure never needs more than
// default handling.
const wchar_t* containerClass()
{
    static const wchar_t* name = [] {
        WNDCLASSEXW wc {};
        wc.cbSize        = sizeof (wc);
        wc.style         = CS_DBLCLKS;
        wc.lpfnWndProc   = &DefWindowProcW;
        wc.hInstance     = ::GetModuleHandleW (nullptr);
        // IDC_ARROW is an integer-resource pseudo-pointer, and without UNICODE
        // defined it expands to the ANSI form; the W call needs it re-cast.
        wc.hCursor       = ::LoadCursorW (nullptr, reinterpret_cast<LPCWSTR> (IDC_ARROW));
        wc.lpszClassName = L"DuskVst3EditorContainer";
        ::RegisterClassExW (&wc);
        return wc.lpszClassName;
    }();
    return name;
}
} // namespace

struct Vst3Editor::Impl
{
    Vst3Editor*      owner    = nullptr;
    Vst3Instance*    instance = nullptr;
    Vst3HostContext* host     = nullptr;

    IPtr<IPlugView> view;
    HWND container = nullptr;

    int   prefW = 0, prefH = 0;
    int   lastW = 0, lastH = 0;
    float contentScale = 0.0f;
    bool  embedded = false, visible = false;

    void confirmSize (int w, int h)
    {
        if (! view || (w == lastW && h == lastH)) return;
        lastW = w;
        lastH = h;
        ViewRect size (0, 0, w, h);
        view->onSize (&size);
    }

    void applyContentScale()
    {
        if (! view || contentScale <= 0.0f) return;
        FUnknownPtr<IPlugViewContentScaleSupport> scale (view);
        if (scale)
            scale->setContentScaleFactor (contentScale);
    }

    bool onResizeView (int w, int h)
    {
        if (w <= 0 || h <= 0 || ! view) return false;
        prefW = w;
        prefH = h;

        if (container != nullptr)
            ::SetWindowPos (container, nullptr, 0, 0, w, h,
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

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

    if (impl->view->isPlatformTypeSupported (kPlatformTypeHWND) != kResultTrue)
    {
        errorOut = "editor does not support HWND embedding";
        impl->view = nullptr;
        return false;
    }

    impl->instance = &inst;
    impl->host = &inst.getHost();
    inst.setResizeViewHandler ([self = impl.get()] (int w, int h)
                               { return self->onResizeView (w, h); });
    impl->view->setFrame (static_cast<IPlugFrame*> (impl->host->plugFrame()));
    inst.setActiveEditorView (impl->view.get());

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
    if (parentHandle == 0) { errorOut = "null parent window"; return false; }

    auto* parent = reinterpret_cast<HWND> (parentHandle);
    const int ww = w > 0 ? w : (impl->prefW > 0 ? impl->prefW : 480);
    const int hh = h > 0 ? h : (impl->prefH > 0 ? impl->prefH : 320);

    // Created hidden; reveal() shows it once the wrapper has final bounds.
    impl->container = ::CreateWindowExW (
        0, containerClass(), L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        x, y, ww, hh, parent, nullptr, ::GetModuleHandleW (nullptr), nullptr);
    if (impl->container == nullptr)
    {
        errorOut = "could not create Win32 container window";
        return false;
    }

    // The scale must reach the view before attached(): a HiDPI-aware plugin
    // sizes its window from the last scale it was told.
    impl->applyContentScale();

    if (impl->view->attached (impl->container, kPlatformTypeHWND) != kResultOk)
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
    if (impl->container == nullptr) return;
    const int ww = std::max (1, w);
    const int hh = std::max (1, h);
    ::SetWindowPos (impl->container, nullptr, x, y, ww, hh,
                    SWP_NOZORDER | SWP_NOACTIVATE);
    if (impl->embedded)
        impl->confirmSize (ww, hh);
}

void Vst3Editor::setContentScale (float scale)
{
    impl->contentScale = scale;
    impl->applyContentScale();
}

void Vst3Editor::reveal()
{
    if (impl->container == nullptr || impl->visible) return;
    ::ShowWindow (impl->container, SW_SHOWNA);
    impl->visible = true;
}

void Vst3Editor::hide()
{
    if (impl->container == nullptr || ! impl->visible) return;
    ::ShowWindow (impl->container, SW_HIDE);
    impl->visible = false;
}

void Vst3Editor::quiesce() noexcept
{
    if (impl->container != nullptr && impl->visible)
        ::ShowWindow (impl->container, SW_HIDE);
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
    // The instance is already disposed but its view class may still own child
    // windows under our container. DestroyWindow would dispatch WM_DESTROY
    // through window procedures in a module that can be unloaded by now, so
    // the container is dropped, not destroyed; the parent peer's teardown
    // reaps it.
    impl->container = nullptr;
    impl->visible   = false;
}

void Vst3Editor::close()
{
    if (impl->instance != nullptr)
    {
        impl->instance->setActiveEditorView (nullptr);
        impl->instance->setResizeViewHandler (nullptr);
        impl->instance = nullptr;
    }
    if (impl->view)
    {
        if (impl->embedded)
            impl->view->removed();
        impl->view->setFrame (nullptr);
        impl->view = nullptr;
    }
    impl->host = nullptr;

    if (impl->container != nullptr)
    {
        ::DestroyWindow (impl->container);
        impl->container = nullptr;
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

#include "ClapEditor.h"

#import <AppKit/AppKit.h>

#include <algorithm>

namespace duskstudio::clap
{
namespace
{
NSView* containerView (std::uintptr_t handle) noexcept
{
    return reinterpret_cast<NSView*> (handle);
}
} // namespace

ClapEditor::~ClapEditor() { close(); }

bool ClapEditor::open (const ::clap_plugin* p, ClapHost& host, std::string& errorOut)
{
    if (p == nullptr) { errorOut = "null plugin"; return false; }

    gui = static_cast<const clap_plugin_gui_t*> (p->get_extension (p, CLAP_EXT_GUI));
    if (gui == nullptr) { errorOut = "plugin has no gui extension"; return false; }
    if (gui->is_api_supported == nullptr
        || ! gui->is_api_supported (p, CLAP_WINDOW_API_COCOA, false))
    { errorOut = "plugin has no embedded-Cocoa GUI"; gui = nullptr; return false; }
    if (gui->create == nullptr || ! gui->create (p, CLAP_WINDOW_API_COCOA, false))
    { errorOut = "gui create() failed"; gui = nullptr; return false; }

    plugin  = p;
    hostPtr = &host;
    host.setPlugin (p);
    host.setCallbacks (this);
    created = true;

    resizable = (gui->can_resize != nullptr) && gui->can_resize (p);

    uint32_t w = 0, h = 0;
    if (gui->get_size != nullptr && gui->get_size (p, &w, &h))
    { prefW = (int) w; prefH = (int) h; }
    return true;
}

bool ClapEditor::embed (void* parentHandle, int x, int y, int w, int h,
                        std::string& errorOut)
{
    if (! created) { errorOut = "gui not created"; return false; }
    if (parentHandle == nullptr) { errorOut = "null parent view"; return false; }

    auto* parent = static_cast<NSView*> (parentHandle);
    const int ww = w > 0 ? w : (prefW > 0 ? prefW : 400);
    const int hh = h > 0 ? h : (prefH > 0 ? prefH : 300);

    auto* container = [[NSView alloc] initWithFrame:NSMakeRect (
        (CGFloat) x, (CGFloat) y, (CGFloat) ww, (CGFloat) hh)];
    if (container == nil) { errorOut = "could not create Cocoa container"; return false; }
    [container setAutoresizingMask:NSViewNotSizable];
    [container setHidden:YES];
    [parent addSubview:container];

    platformContext = parent;
    containerHandle = reinterpret_cast<std::uintptr_t> (container);

    clap_window_t win {};
    win.api = CLAP_WINDOW_API_COCOA;
    win.cocoa = (clap_nsview) container;
    if (gui->set_parent == nullptr || ! gui->set_parent (plugin, &win))
    { errorOut = "gui set_parent() failed"; close(); return false; }

    if (resizable && gui->set_size != nullptr)
        gui->set_size (plugin, (uint32_t) ww, (uint32_t) hh);
    if (gui->show != nullptr && ! gui->show (plugin))
    { errorOut = "gui show() failed"; close(); return false; }

    embedded = true;
    return true;
}

void ClapEditor::setBounds (int x, int y, int w, int h)
{
    auto* container = containerView (containerHandle);
    if (container == nil) return;

    const int ww = std::max (1, w);
    const int hh = std::max (1, h);
    [container setFrame:NSMakeRect ((CGFloat) x, (CGFloat) y, (CGFloat) ww, (CGFloat) hh)];
    if (resizable && gui != nullptr && gui->set_size != nullptr && w > 0 && h > 0)
        gui->set_size (plugin, (uint32_t) w, (uint32_t) h);
}

bool ClapEditor::getRootRelativePosition (void*, int&, int&) const
{
    return false;
}

bool ClapEditor::getActualGeometry (int&, int&, int&, int&) const
{
    return false;
}

void ClapEditor::reveal()
{
    auto* container = containerView (containerHandle);
    if (container == nil || mapped) return;
    [container setHidden:NO];
    mapped = true;
}

void ClapEditor::hide()
{
    auto* container = containerView (containerHandle);
    if (container == nil || ! mapped) return;
    [container setHidden:YES];
    mapped = false;
}

void ClapEditor::abandonPluginAndContainer() noexcept
{
    abandonPlugin();
    // Hide rather than detach: the plugin's view is still a subview and must not
    // leave the window.
    @try { if (mapped) [containerView (containerHandle) setHidden:YES]; }
    @catch (NSException*) {}
    platformContext = nullptr;
    containerHandle = 0;
    mapped          = false;
}

void ClapEditor::close()
{
    // Leak path: the plugin GUI stays created (it hangs in gui->destroy) and keeps
    // drawing into our container view, so the view is leaked with it rather than
    // released out from under a live GUI.
    if (leakOnClose && created)
    {
        if (hostPtr != nullptr)
        {
            hostPtr->markGuiLeaked();
            hostPtr->setCallbacks (nullptr);
            hostPtr = nullptr;
        }
        // Detach without releasing: left as a subview, the peer's dealloc would
        // release the container out from under the live GUI.
        if (auto* container = containerView (containerHandle); container != nil)
            [container removeFromSuperview];
        platformContext = nullptr;
        containerHandle = 0;
        gui = nullptr;
        plugin = nullptr;
        created = embedded = mapped = false;
        return;
    }

    if (plugin != nullptr && gui != nullptr)
    {
        if (gui->hide != nullptr)    gui->hide (plugin);
        if (gui->destroy != nullptr) gui->destroy (plugin);
    }
    if (hostPtr != nullptr) { hostPtr->setCallbacks (nullptr); hostPtr = nullptr; }

    if (auto* container = containerView (containerHandle); container != nil)
    {
        [container removeFromSuperview];
        [container release];
    }
    platformContext = nullptr;
    containerHandle = 0;
    gui = nullptr;
    plugin = nullptr;
    created = embedded = mapped = false;
}

void ClapEditor::drainPendingCallbacks()
{
    if (pendingClosed.exchange (false))
    {
        if (pendingClosedWasDestroyed.load())
        {
            if (plugin != nullptr && gui != nullptr && gui->destroy != nullptr)
                gui->destroy (plugin);
            gui = nullptr;
            created = embedded = false;
            hide();
        }
        if (onClosed) onClosed();
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
        if (auto* container = containerView (containerHandle); container != nil)
        {
            auto frame = [container frame];
            frame.size = NSMakeSize ((CGFloat) std::max (1, w),
                                     (CGFloat) std::max (1, h));
            [container setFrame:frame];
        }
        if (onResize) onResize (w, h);
    }

    if (pendingShow.exchange (false)) reveal();
    if (pendingHide.exchange (false)) hide();
}

void ClapEditor::pump (double elapsedMs)
{
    drainPendingCallbacks();
    if (hostPtr != nullptr) hostPtr->pumpGui (elapsedMs);
}

bool ClapEditor::onRequestResize (uint32_t w, uint32_t h)
{
    pendingW.store ((int) w, std::memory_order_relaxed);
    pendingH.store ((int) h, std::memory_order_relaxed);
    pendingResize.store (true, std::memory_order_release);
    return true;
}

bool ClapEditor::onRequestShow()
{
    pendingShow.store (true, std::memory_order_release);
    return true;
}

bool ClapEditor::onRequestHide()
{
    pendingHide.store (true, std::memory_order_release);
    return true;
}

void ClapEditor::onGuiClosed (bool wasDestroyed)
{
    pendingClosedWasDestroyed.store (wasDestroyed, std::memory_order_relaxed);
    pendingClosed.store (true, std::memory_order_release);
}
} // namespace duskstudio::clap

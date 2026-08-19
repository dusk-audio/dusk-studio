// Ahead of every include: whichever header reaches windows.h first must see
// these, or its min/max macros eat std::max at the call sites below.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ClapEditor.h"

#include <algorithm>

namespace duskstudio::clap
{
namespace
{
HWND containerHwnd (std::uintptr_t handle) noexcept
{
    return reinterpret_cast<HWND> (handle);
}

// One process-wide class for every CLAP container window. The plugin parents
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
        wc.lpszClassName = L"DuskClapEditorContainer";
        ::RegisterClassExW (&wc);
        return wc.lpszClassName;
    }();
    return name;
}
} // namespace

ClapEditor::~ClapEditor() { close(); }

bool ClapEditor::open (const ::clap_plugin* p, ClapHost& host, std::string& errorOut)
{
    if (p == nullptr) { errorOut = "null plugin"; return false; }

    gui = static_cast<const clap_plugin_gui_t*> (p->get_extension (p, CLAP_EXT_GUI));
    if (gui == nullptr) { errorOut = "plugin has no gui extension"; return false; }
    if (gui->is_api_supported == nullptr
        || ! gui->is_api_supported (p, CLAP_WINDOW_API_WIN32, false))
    { errorOut = "plugin has no embedded-Win32 GUI"; gui = nullptr; return false; }
    if (gui->create == nullptr || ! gui->create (p, CLAP_WINDOW_API_WIN32, false))
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
    if (parentHandle == nullptr) { errorOut = "null parent window"; return false; }

    auto* parent = static_cast<HWND> (parentHandle);
    const int ww = w > 0 ? w : (prefW > 0 ? prefW : 400);
    const int hh = h > 0 ? h : (prefH > 0 ? prefH : 300);

    // Created hidden; reveal() shows it once the wrapper has final bounds.
    HWND container = ::CreateWindowExW (
        0, containerClass(), L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        x, y, ww, hh, parent, nullptr, ::GetModuleHandleW (nullptr), nullptr);
    if (container == nullptr)
    { errorOut = "could not create Win32 container window"; return false; }

    platformContext = parent;
    containerHandle = reinterpret_cast<std::uintptr_t> (container);

    clap_window_t win {};
    win.api   = CLAP_WINDOW_API_WIN32;
    win.win32 = container;
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
    HWND container = containerHwnd (containerHandle);
    if (container == nullptr) return;

    const int ww = std::max (1, w);
    const int hh = std::max (1, h);
    ::SetWindowPos (container, nullptr, x, y, ww, hh,
                    SWP_NOZORDER | SWP_NOACTIVATE);
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
    HWND container = containerHwnd (containerHandle);
    if (container == nullptr || mapped) return;
    ::ShowWindow (container, SW_SHOWNA);
    mapped = true;
}

void ClapEditor::hide()
{
    HWND container = containerHwnd (containerHandle);
    if (container == nullptr || ! mapped) return;
    ::ShowWindow (container, SW_HIDE);
    mapped = false;
}

void ClapEditor::quiesce() noexcept
{
    if (HWND container = containerHwnd (containerHandle); container != nullptr && mapped)
        ::ShowWindow (container, SW_HIDE);
    mapped = false;
}

void ClapEditor::abandonPluginAndContainer() noexcept
{
    abandonPlugin();
    // The plugin is already disposed but its window class may still own child
    // windows under our container. DestroyWindow would dispatch WM_DESTROY
    // through window procedures in a DLL that can be unloaded by now, so the
    // container is dropped, not destroyed; the parent peer's teardown reaps it.
    platformContext = nullptr;
    containerHandle = 0;
    mapped          = false;
}

void ClapEditor::close()
{
    // Leak path: the plugin GUI stays created (it hangs in gui->destroy) and
    // keeps its child windows under our container, so the container HWND is
    // leaked with it; the peer's own destruction tears the window tree down
    // while the leaked GUI is still a valid message target.
    if (leakOnClose && created)
    {
        if (hostPtr != nullptr)
        {
            hostPtr->markGuiLeaked();
            hostPtr->setCallbacks (nullptr);
            hostPtr = nullptr;
        }
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

    if (HWND container = containerHwnd (containerHandle); container != nullptr)
        ::DestroyWindow (container);
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
        if (HWND container = containerHwnd (containerHandle); container != nullptr)
            ::SetWindowPos (container, nullptr, 0, 0,
                            std::max (1, w), std::max (1, h),
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
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

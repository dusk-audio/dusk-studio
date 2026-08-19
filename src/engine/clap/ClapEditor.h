#pragma once

#include "ClapHost.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace duskstudio::clap
{
// Native embedded editor for a CLAP plugin. The platform translation unit owns
// an X11 child window on Linux, an NSView child container on macOS, or a child
// HWND on Windows. Drives the plugin's event pump through ClapHost on the
// message thread.
//
// Platform headers do not leak through this boundary. Native parent handles are
// opaque pointers, so the API also remains safe on pointer-width Windows handles.
class ClapEditor : public ClapHost::Callbacks
{
public:
    ClapEditor() = default;
    ~ClapEditor() override;
    ClapEditor (const ClapEditor&)            = delete;
    ClapEditor& operator= (const ClapEditor&) = delete;

    // The plugin must be created + activated. Creates the platform CLAP GUI and
    // queries its preferred size. False (+errorOut) if no compatible embedded
    // GUI exists or create() fails.
    bool open (const ::clap_plugin* plugin, ClapHost& host, std::string& errorOut);

    // Create a native child container under parentHandle at (x,y,w,h), embed the
    // plugin into it, and call the plugin's show(). reveal() controls whether the
    // container itself is visible.
    bool embed (void* parentHandle, int x, int y, int w, int h, std::string& errorOut);

    void setBounds (int x, int y, int w, int h);

    // Tell the GUI the host's logical->physical factor. A HiDPI-aware plugin
    // reports its size in physical pixels only once it knows the scale, so the
    // cached preferred size is re-read when it takes. CLAP fixes the macOS
    // scale at 1, so only the Win32 embed calls this.
    void setContentScale (double scale) noexcept
    {
        if (! created || plugin == nullptr || gui == nullptr) return;
        if (gui->set_scale == nullptr || scale <= 0.0) return;
        if (! gui->set_scale (plugin, scale)) return;

        uint32_t w = 0, h = 0;
        if (gui->get_size != nullptr && gui->get_size (plugin, &w, &h) && w > 0 && h > 0)
        { prefW = (int) w; prefH = (int) h; }
    }

    void reveal();
    void hide();

    // Hide the host container while the plugin/module are still valid. Cocoa
    // abandonment must quiesce before disposal because setHidden: notifies the
    // embedded plugin view.
    void quiesce() noexcept;
    void close();

    // Linux geometry diagnostics. macOS uses logical component bounds directly,
    // so these return false there.
    bool getRootRelativePosition (void* referenceHandle,
                                  int& relX, int& relY) const;
    bool getActualGeometry (int& x, int& y, int& w, int& h) const;

    // App shutdown: leak the plugin GUI instead of destroying it. u-he (Satin/Diva)
    // hang in gui->destroy on teardown - same reason the JUCE host leaks plugins on
    // quit. close() then skips gui->hide/gui->destroy; the process is exiting anyway.
    void setLeakOnClose (bool b) noexcept { leakOnClose = b; }

    // The plugin is going away but is STILL ALIVE. Drop every plugin-side handle
    // WITHOUT calling through it - the vtables go with the bundle - so close()
    // only releases the container this host owns, while that plugin is valid.
    void abandonPlugin() noexcept
    {
        plugin  = nullptr;
        gui     = nullptr;
        hostPtr = nullptr;
        created = embedded = false;
    }

    // Same, for an ALREADY-destroyed plugin: only clear retained references on
    // Cocoa. No late setHidden:, detach or release may message the embedded view.
    // X11 has no in-process child-view hazard, so it still closes its host window
    // and display after dropping the plugin handles.
    void abandonPluginAndContainer() noexcept;

    // Message thread, ~60 Hz: apply queued GUI callbacks, pump the plugin's
    // fds/timers, and drain any platform editor events.
    void pump (double elapsedMs);

    int  preferredWidth()  const noexcept { return prefW; }
    int  preferredHeight() const noexcept { return prefH; }
    bool isOpen()      const noexcept { return created; }
    bool isEmbedded()  const noexcept { return embedded; }

    // Set by the JUCE wrapper: the plugin asked to resize / the GUI closed.
    std::function<void (int, int)> onResize;
    std::function<void()>          onClosed;

    // ClapHost::Callbacks
    bool onRequestResize (uint32_t w, uint32_t h) override;
    bool onRequestShow() override;
    bool onRequestHide() override;
    void onGuiClosed (bool wasDestroyed) override;

private:
    // The plugin's gui host callbacks are [thread-safe] - it may call request_resize/
    // show/hide/closed from a non-message thread. Those handlers only stash these
    // atomics; drainPendingCallbacks() (from pump(), on the message thread) does all
    // the native + JUCE work, so nothing touches a native view or a JUCE
    // Component off-thread.
    void drainPendingCallbacks();

    const ::clap_plugin*     plugin = nullptr;
    const clap_plugin_gui_t* gui    = nullptr;
    ClapHost* hostPtr = nullptr;

    void*          platformContext = nullptr; // Display* on Linux; parent NSView* on macOS
    std::uintptr_t containerHandle = 0;       // X11 Window or owned NSView*
    int  prefW = 0, prefH = 0;
    bool created = false, embedded = false, mapped = false;
    bool resizable = false;   // gui->can_resize: calling set_size when false aborts some plugins (u-he)
    bool leakOnClose = false; // shutdown: skip gui->destroy (u-he hangs there)

    std::atomic<bool> pendingResize { false };
    std::atomic<int>  pendingW { 0 }, pendingH { 0 };
    std::atomic<bool> pendingShow   { false };
    std::atomic<bool> pendingHide   { false };
    std::atomic<bool> pendingClosed { false };
    std::atomic<bool> pendingClosedWasDestroyed { false };
};
} // namespace duskstudio::clap

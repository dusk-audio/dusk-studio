#pragma once

#include "ClapHost.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace duskstudio::clap
{
// Native embedded editor for a CLAP plugin. The platform translation unit owns
// an X11 child window on Linux or an NSView child container on macOS. Drives the
// plugin's fd/timer event pump through ClapHost on the message thread.
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
    void reveal();
    void hide();
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

    // Same, for an ALREADY-destroyed plugin: nothing may reach plugin code
    // again. On Cocoa that costs the container as well - close() releases it
    // with the plugin's view still inside and nothing else retaining that view,
    // so the release can run its -dealloc against a disposed instance in an
    // unloaded module. It is hidden and then given up unreleased instead: that
    // trades one bounded viewDidHide for the unbounded drawRect/displayLayer a
    // parented dead view takes on every window redraw, and the mapped guard
    // means the usual reap (lane already hidden) sends no message at all. What
    // it does not fix is the leaked view's own timers and listeners. Each stale
    // reap strands one container plus the plugin's entire view hierarchy for the
    // life of the process, so repeated undo/redo or session loads with an editor
    // open accumulate them. X11 has no such hazard (the plugin's window is a
    // foreign child): there this is exactly abandonPlugin(), and close() still
    // destroys the host window and display.
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

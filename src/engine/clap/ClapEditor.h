#pragma once

#include "ClapHost.h"

#include <atomic>
#include <functional>
#include <string>

namespace duskstudio::clap
{
// Native X11 editor embed for a CLAP plugin (Linux). Owns a host X11 window
// created UNMAPPED as a child of the app's window; the plugin embeds its GUI into
// it (set_parent + show), and we XMapWindow it ONLY on reveal() — so the first
// open is an instant map with no stray-window flash, by construction. Drives the
// plugin's fd/timer event pump (via ClapHost) on the message thread.
//
// No X11 headers leak here: the host window + display are opaque handles.
class ClapEditor : public ClapHost::Callbacks
{
public:
    ClapEditor() = default;
    ~ClapEditor() override;
    ClapEditor (const ClapEditor&)            = delete;
    ClapEditor& operator= (const ClapEditor&) = delete;

    // The plugin must be created + activated. Creates the embedded-X11 CLAP GUI
    // and queries its preferred size. False (+errorOut) if the plugin has no
    // embedded-X11 GUI or create() fails.
    bool open (const ::clap_plugin* plugin, ClapHost& host, std::string& errorOut);

    // Create our host X11 window under parentX11 at (x,y,w,h) UNMAPPED, embed the
    // plugin into it, and show() the plugin. Nothing is mapped on-screen yet.
    bool embed (unsigned long parentX11, int x, int y, int w, int h, std::string& errorOut);

    void setBounds (int x, int y, int w, int h);
    void reveal();   // XMapWindow — the instant first-open
    void hide();     // XUnmapWindow
    void close();

    // Message thread, ~60 Hz: pump the plugin's fds/timers + drain our X events.
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
    // The plugin's gui host callbacks are [thread-safe] — it may call request_resize/
    // show/hide/closed from a non-message thread. Those handlers only stash these
    // atomics; drainPendingCallbacks() (from pump(), on the message thread) does all
    // the X11 + JUCE work, so nothing touches Xlib / a JUCE Component off-thread.
    void drainPendingCallbacks();

    const ::clap_plugin*     plugin = nullptr;
    const clap_plugin_gui_t* gui    = nullptr;
    ClapHost* hostPtr = nullptr;

    void*         display    = nullptr;   // Display* (opaque)
    unsigned long hostWindow = 0;         // Window
    int  prefW = 0, prefH = 0;
    bool created = false, embedded = false, mapped = false;

    std::atomic<bool> pendingResize { false };
    std::atomic<int>  pendingW { 0 }, pendingH { 0 };
    std::atomic<bool> pendingShow   { false };
    std::atomic<bool> pendingHide   { false };
    std::atomic<bool> pendingClosed { false };
    std::atomic<bool> pendingClosedWasDestroyed { false };
};
} // namespace duskstudio::clap

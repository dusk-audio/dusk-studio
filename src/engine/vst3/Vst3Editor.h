#pragma once

#include <functional>
#include <memory>
#include <string>

namespace duskstudio::vst3
{
class Vst3Instance;

// Native X11 editor embed for a VST3 plugin view (Linux). Owns a host X11
// window created as a child of the app's window; the controller's IPlugView
// attaches to it via kPlatformTypeX11EmbedWindowID. Mirrors Lv2Editor's shape:
// open() discovers/creates the view, embed() parents it on-screen.
//
// The frame is wired at open() — BEFORE attached() — so the view can reach the
// IPlugFrame (and query the Linux IRunLoop through it) while attaching. Resize
// requests round-trip resizeView → host window resize → onSize, per spec.
//
// No X11 / Steinberg types leak here: everything lives behind the pImpl.
class Vst3Editor
{
public:
    Vst3Editor();
    ~Vst3Editor();
    Vst3Editor (const Vst3Editor&)            = delete;
    Vst3Editor& operator= (const Vst3Editor&) = delete;

    // Create the controller's editor view and wire the frame + host callbacks.
    // False (+errorOut) when the plugin ships no X11-embeddable editor. Keeps a
    // reference to `inst` — the owner must keep it alive past this editor.
    bool open (Vst3Instance& inst, std::string& errorOut);

    // Create the host X11 window under parentX11 at (x,y,w,h), attach the view
    // into it, and map it.
    bool embed (unsigned long parentX11, int x, int y, int w, int h, std::string& errorOut);

    void setBounds (int x, int y, int w, int h);
    void setContentScale (float scale);
    void reveal();   // XMapWindow (idempotent)
    void hide();     // XUnmapWindow (idempotent)
    void close();

    // The host window's REAL geometry (position relative to its X11 parent +
    // size), so the owner can detect and correct drift the message flow
    // missed. False when not embedded or the window is gone.
    bool getActualGeometry (int& x, int& y, int& w, int& h) const;

    // Message thread, ~60 Hz: pump the instance's IRunLoop (fds + timers) and
    // drain our X connection.
    void pump (double elapsedMs);

    int  preferredWidth()  const noexcept;
    int  preferredHeight() const noexcept;
    bool isOpen()     const noexcept;
    bool isEmbedded() const noexcept;

    // The view asked to resize (IPlugFrame::resizeView).
    std::function<void (int, int)> onResize;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::vst3

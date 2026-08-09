#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace duskstudio::vst3
{
class Vst3Instance;

// Native editor embed for a VST3 plugin view. The platform translation unit
// owns an X11 child window on Linux or an NSView child container on macOS.
// open() discovers/creates the view; embed() attaches it to the native child.
//
// The frame is wired at open() - BEFORE attached() - so the view can reach the
// IPlugFrame while attaching. On Linux it can also query the IRunLoop. Resize
// requests round-trip resizeView -> host container resize -> onSize, per spec.
//
// No platform / Steinberg types leak here: everything lives behind the pImpl.
// Native handles use uintptr_t rather than X11's unsigned long, keeping the
// boundary pointer-width-safe on every platform.
class Vst3Editor
{
public:
    Vst3Editor();
    ~Vst3Editor();
    Vst3Editor (const Vst3Editor&)            = delete;
    Vst3Editor& operator= (const Vst3Editor&) = delete;

    // Create the controller's editor view and wire the frame + host callbacks.
    // False (+errorOut) when the plugin ships no compatible embedded editor.
    // Keeps a reference to `inst` - the owner must keep it alive past this editor.
    bool open (Vst3Instance& inst, std::string& errorOut);

    // Create a native child container under parentHandle at (x,y,w,h) and
    // attach the view into it. Visibility afterwards is platform-specific: the
    // X11 host window is mapped here (toolkit editors can refuse to realise
    // into an unmapped parent), while the Cocoa container stays hidden until
    // reveal(). reveal()/hide() are idempotent on both.
    bool embed (std::uintptr_t parentHandle, int x, int y, int w, int h,
                std::string& errorOut);

    void setBounds (int x, int y, int w, int h);
    void setContentScale (float scale);
    void reveal();
    void hide();
    void close();

    // The instance is going away but is STILL ALIVE. Drops the view without
    // releasing it and without the removed()/setFrame() teardown - the module
    // those calls live in goes with the instance. close() then only releases the
    // host container, whose detach still reaches a valid plugin.
    void abandonPlugin() noexcept;

    // Same, for an instance that is ALREADY destroyed: nothing may reach plugin
    // code again. On Cocoa that costs the container as well - the plugin's view
    // is still a subview, so detaching or releasing it runs that view's
    // window-detach hooks against the dead instance. It is hidden and then given
    // up unreleased instead: that trades one bounded viewDidHide for the
    // unbounded drawRect/displayLayer a parented dead view takes on every window
    // redraw, and the visible guard means the usual reap (lane already hidden)
    // sends no message at all. What it does not fix is the leaked view's own
    // timers and listeners, and its IPlugFrame pointer, which can no longer be
    // cleared. Each stale reap strands one container plus the plugin's entire
    // view hierarchy for the life of the process, so repeated undo/redo or
    // session loads with an editor open accumulate them. X11 has no such hazard
    // (the plugin's window is a foreign child): there this is exactly
    // abandonPlugin(), and close() still destroys the host window and display.
    void abandonPluginAndContainer() noexcept;

    // Linux geometry diagnostics. macOS uses logical component bounds directly,
    // so these return false there.
    bool getRootRelativePosition (std::uintptr_t referenceHandle,
                                  int& relX, int& relY) const;
    bool getActualGeometry (int& x, int& y, int& w, int& h) const;

    // Message thread, ~60 Hz: pump the instance's host context and drain any
    // platform editor events.
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

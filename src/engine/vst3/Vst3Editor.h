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

    // Hide the host container while the instance/module are still valid. Cocoa
    // abandonment must quiesce before disposal because setHidden: notifies the
    // embedded plugin view.
    void quiesce() noexcept;
    void close();

    // The instance is going away but is STILL ALIVE. Drops the view without
    // releasing it and without the removed()/setFrame() teardown - the module
    // those calls live in goes with the instance. close() then only releases the
    // host container, whose detach still reaches a valid plugin.
    void abandonPlugin() noexcept;

    // Same, for an ALREADY-destroyed instance: only clear retained references on
    // Cocoa. No late setHidden:, detach or release may message the embedded view.
    // X11 has no in-process child-view hazard, so it still closes its host window
    // and display after dropping the plugin handles.
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

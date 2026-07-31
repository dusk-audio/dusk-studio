#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace duskstudio::lv2
{
class Lv2Instance;

// Native editor embed for an LV2 plugin UI via suil. Owns an X11 child window
// on Linux or an NSView child container on macOS and drives the UI's
// ui:idleInterface from pump() on the message thread.
//
// Lifecycle mirrors ClapEditor with one structural difference: an LV2 UI takes
// its ui:parent at instantiate time, so open() only discovers the UI and the
// real instantiation happens in embed() - there is no pre-built unmapped stage.
//
// No X11 / lilv / suil types leak here: everything lives behind the pImpl.
class Lv2Editor
{
public:
    Lv2Editor();
    ~Lv2Editor();
    Lv2Editor (const Lv2Editor&)            = delete;
    Lv2Editor& operator= (const Lv2Editor&) = delete;

    // Discover an embeddable UI for the (created + activated) instance. False
    // (+errorOut) when the plugin ships no UI compatible with this platform.
    // Keeps a reference to `inst` - the slot owning it must outlive this editor.
    bool open (Lv2Instance& inst, std::string& errorOut);

    // Create the native child container under parentHandle at (x,y,w,h),
    // instantiate the UI into it via suil, and make it ready for reveal().
    bool embed (std::uintptr_t parentHandle, int x, int y, int w, int h,
                std::string& errorOut);

    void setBounds (int x, int y, int w, int h);
    void reveal();   // Show the native child container (idempotent).
    void hide();     // Hide the native child container (idempotent).
    void close();

    // The host window's REAL geometry (position relative to its X11 parent +
    // size), so the owner can detect and correct drift the message flow
    // missed. False when not embedded or the window is gone.
    bool getRootRelativePosition (std::uintptr_t referenceHandle,
                                  int& relX, int& relY) const;
    bool getActualGeometry (int& x, int& y, int& w, int& h) const;

    // App shutdown: skip suil_instance_free - a foreign-toolkit UI's destructor
    // can hang on the way out (same rationale as the CLAP editor leak path).
    void setLeakOnClose (bool b) noexcept;

    // Message thread, ~60 Hz: drive ui:idleInterface and platform event work.
    void pump();

    int  preferredWidth()  const noexcept;
    int  preferredHeight() const noexcept;
    bool isOpen()     const noexcept;
    bool isEmbedded() const noexcept;

    // The UI asked to resize (ui:resize host feature) / reported it closed.
    std::function<void (int, int)> onResize;
    std::function<void()>          onClosed;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::lv2

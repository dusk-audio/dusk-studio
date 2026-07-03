#pragma once

#include <functional>
#include <memory>
#include <string>

namespace duskstudio::lv2
{
class Lv2Instance;

// Native X11 editor embed for an LV2 plugin UI via suil (Linux). Owns a host X11
// window created as a child of the app's window; suil instantiates the plugin's
// UI into it (X11UI natively, foreign toolkits through suil's wrapper modules)
// and the UI's ui:idleInterface is driven from pump() on the message thread.
//
// Lifecycle mirrors ClapEditor with one structural difference: an LV2 UI takes
// its ui:parent at instantiate time, so open() only discovers the UI and the
// real instantiation happens in embed() — there is no pre-built unmapped stage.
//
// No X11 / lilv / suil types leak here: everything lives behind the pImpl.
class Lv2Editor
{
public:
    Lv2Editor();
    ~Lv2Editor();
    Lv2Editor (const Lv2Editor&)            = delete;
    Lv2Editor& operator= (const Lv2Editor&) = delete;

    // Discover an embeddable UI for the (created + activated) instance: native
    // X11UI preferred, else the best UI suil can wrap into X11. False (+errorOut)
    // when the plugin ships no usable UI. Keeps a reference to `inst` — the slot
    // owning it must outlive this editor.
    bool open (Lv2Instance& inst, std::string& errorOut);

    // Create the host X11 window under parentX11 at (x,y,w,h), instantiate the
    // UI into it via suil, and map it.
    bool embed (unsigned long parentX11, int x, int y, int w, int h, std::string& errorOut);

    void setBounds (int x, int y, int w, int h);
    void reveal();   // XMapWindow (idempotent)
    void hide();     // XUnmapWindow (idempotent)
    void close();

    // The host window's REAL geometry (position relative to its X11 parent +
    // size), so the owner can detect and correct drift the message flow
    // missed. False when not embedded or the window is gone.
    bool getActualGeometry (int& x, int& y, int& w, int& h) const;

    // App shutdown: skip suil_instance_free — a foreign-toolkit UI's destructor
    // can hang on the way out (same rationale as the CLAP editor leak path).
    void setLeakOnClose (bool b) noexcept;

    // Message thread, ~60 Hz: drive ui:idleInterface + drain our X connection.
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

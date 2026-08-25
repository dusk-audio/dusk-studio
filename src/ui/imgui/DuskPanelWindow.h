#pragma once

#include <DuskWidgets.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace duskstudio::imgui
{
// The transport and navigation keys a modal keeps live for the DAW behind it. The
// shell binds them; a native view has no way to name a JUCE key press, so it reports
// which of these the user asked for and the shell turns that back into its own
// shortcut. The set is deliberately the one the JUCE modals forward: transport,
// loop and punch, playhead home, fullscreen. Nothing that edits the arrangement
// hidden behind the panel.
enum class ShellShortcut
{
    playStop,
    record,
    playheadToZero,
    stopAndRewind,
    toggleLoop,
    togglePunch,
    setLoopIn,
    setLoopOut,
    setPunchIn,
    setPunchOut,
    toggleFullscreen
};

// What a native panel implements. The view owns its parameters and draws into the
// frame the window gives it; everything around the body - the dim, the panel plate,
// dismissal, the shortcut gate - belongs to the window.
class DuskPanelView
{
public:
    virtual ~DuskPanelView() = default;

    // The body's size in design pixels, before the window's scale. Read every frame,
    // so a view whose layout depends on its own state (the comp editor's mode row)
    // may change it between frames.
    virtual ImVec2 preferredSize() const = 0;

    // Draw the body. `origin` is the body's top-left in window coordinates and
    // `size` is what the window could actually grant, which is the preferred size
    // unless the host window is too small for it.
    virtual void draw (DuskWidgets::Context& ctx, ImVec2 origin, ImVec2 size) = 0;

    // How dark the DAW behind the panel goes. The processing editors use the lighter
    // 0.28 so the strip meters stay readable while auditioning; decision panels keep
    // the 0.55 default.
    virtual float dimAlpha() const { return 0.55f; }

    // A view that binds a bare transport key to its own input - the virtual
    // keyboard's note letters - claims it here, and the window stops forwarding it.
    // Claim implies consume: a claimed key never reaches the shell.
    virtual bool claimsShortcut (ShellShortcut) const { return false; }

    // False keeps Escape inside the view (a text field being edited, a popup that
    // should close first). The window dismisses on Escape only when this is true.
    virtual bool escapeDismisses() const { return true; }
};

// A native modal panel over the JUCE shell.
//
// The child window covers the whole host window rather than just the panel plate, so
// the dim, the click-outside test and any popup the body opens are all inside one
// framework surface: a combo dropped near the panel's edge has the rest of the window
// to open into, and no JUCE sibling has to be kept in sync with the child's rectangle.
// It is also the shape G5 ends at, where the shell itself is this window.
class DuskPanelWindow final
{
public:
    struct Geometry
    {
        int x = 0;
        int y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        double scaleFactor = 1.0;
    };

    struct Callbacks
    {
        // Escape, or a click outside the panel plate. The caller decides what that
        // means; close() is not implied.
        std::function<void()> dismissed;

        // The window has finished closing, whether the caller asked for it or a
        // graphics failure forced it.
        std::function<void()> closed;

        // A transport key the panel did not claim. Return true if the shell took it.
        std::function<bool (ShellShortcut)> shortcut;
    };

    // `logTag` prefixes this window's diagnostics and names its first-frame marker,
    // so one panel that ends a run inside its first frame does not refuse the others.
    DuskPanelWindow (std::string className, std::string logTag, std::string displayName);
    ~DuskPanelWindow();

    void setCallbacks (Callbacks callbacks);

    // The view is built before open() and released when the window closes.
    void setView (std::unique_ptr<DuskPanelView> view);

    bool open (std::uintptr_t nativeParent, Geometry geometry);

    // Why the last open() returned false, phrased for the user. Empty when it
    // succeeded or was never called.
    const std::string& lastOpenFailure() const noexcept;

    void setGeometry (Geometry geometry);
    void close();
    bool isOpen() const noexcept;

    // What a file dialog opened through xdg-desktop-portal should be parented to:
    // "x11:<id>", "wayland:<handle>", or empty where the backend has none.
    std::string portalParentHandle() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::imgui

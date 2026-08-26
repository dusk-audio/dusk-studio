#pragma once

#include "ShellShortcut.h"

#include <DuskWidgets.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace duskstudio::imgui
{
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

    // How dark the DAW behind the panel goes. A framework child is opaque, so the
    // dim is the host's to paint behind it; this is what the panel asks for. The
    // processing editors use the lighter 0.28 so the strip meters stay readable
    // while auditioning; decision panels keep the 0.55 default.
    virtual float dimAlpha() const { return 0.55f; }

    // False for a view that is not a modal - an inline editor sitting inside a stage
    // rather than floating over one. The window then draws no plate and hands the view
    // its whole rectangle, which the view fills with its own panel surface.
    virtual bool wantsPlate() const { return true; }

    // A view that binds a bare transport key to its own input - the virtual
    // keyboard's note letters - claims it here, and the window stops forwarding it.
    // Claim implies consume: a claimed key never reaches the shell.
    virtual bool claimsShortcut (ShellShortcut) const { return false; }

    // False keeps Escape inside the view (a text field being edited, a popup that
    // should close first). The window dismisses on Escape only when this is true.
    virtual bool escapeDismisses() const { return true; }

    // A view that closes itself - a Cancel button, or the shortcut that opened it
    // pressed again - raises this and the window dismisses. Cleared by the read,
    // because a framework child has no parent chain for an unhandled key to walk
    // the way a JUCE modal body did.
    virtual bool takeDismissRequest() { return false; }

    // A view that draws a bitmap puts it in the font atlas, the way the knob's baked
    // dome does, so it costs a quad and no draw call of its own. Reserve runs before
    // the atlas is packed and rasterise after it is built and before the texture is
    // uploaded; both are called again whenever the atlas is rebuilt for a new scale.
    virtual void reserveAtlasImages (ImFontAtlas&) {}
    virtual void rasteriseAtlasImages (ImFontAtlas&) {}
};

// One RGBA bitmap living in a view's slice of the font atlas. The pixels are the
// caller's and are only read during rasterise.
class AtlasImage
{
public:
    // `rgba` is width * height * 4 bytes, straight RGBA. Nothing is copied.
    void reserve (ImFontAtlas& atlas, const unsigned char* rgba, int width, int height);
    void rasterise (ImFontAtlas& atlas);

    bool ready() const noexcept;
    ImTextureID textureId() const noexcept;
    ImVec2 uvMin() const noexcept { return uv[0]; }
    ImVec2 uvMax() const noexcept { return uv[1]; }

    // Draw it into `tl`..`br`, letterboxed so the source aspect is kept.
    void draw (ImDrawList& dl, ImVec2 tl, ImVec2 br) const;

private:
    const ImFontAtlas* source = nullptr;
    const unsigned char* pixels = nullptr;
    ImVec2 uv[2] = {};
    int rect = -1;
    int imageWidth = 0;
    int imageHeight = 0;
};

// A native modal panel over the JUCE shell.
//
// The child covers the panel's plate and nothing more, because a framework child is
// an opaque native surface: one sized to the whole window would paint black over the
// DAW instead of dimming it. The dim and the click-outside test therefore stay on the
// host's side, exactly as the session notepad already arranges them, and the caller
// keeps a dim overlay behind the child sized from plateSize().
class DuskPanelWindow final
{
public:
    // The plate's size in design pixels: the body its view asks for plus the frame
    // drawn around it. What the caller centres in the host window. Zero until a view
    // is set.
    struct PlateSize
    {
        int width = 0;
        int height = 0;
    };

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

        // Where the child belongs now. Polled while the window is open so a host
        // resized underneath it follows without every caller having to listen for
        // one; leaving it empty pins the child to the geometry open() was given.
        std::function<Geometry()> geometry;
    };

    // `logTag` prefixes this window's diagnostics and names its first-frame marker,
    // so one panel that ends a run inside its first frame does not refuse the others.
    DuskPanelWindow (std::string className, std::string logTag, std::string displayName);
    ~DuskPanelWindow();

    void setCallbacks (Callbacks callbacks);

    // The view is built before open() and released when the window closes.
    void setView (std::unique_ptr<DuskPanelView> view);

    PlateSize plateSize() const;

    // What the host should paint behind the child, from the current view.
    float dimAlpha() const;

    // Write the next steady frame to `path` as a PPM, once, then forget it. How a
    // native panel reaches the manual's figures: the JUCE screenshot harness cannot
    // snapshot a framework child, so the application reads its own frame back.
    void captureNextFrameTo (std::string path);

    bool open (std::uintptr_t nativeParent, Geometry geometry);

    // Why the last open() returned false, phrased for the user. Empty when it
    // succeeded or was never called.
    const std::string& lastOpenFailure() const noexcept;

    void setGeometry (Geometry geometry);
    void close();
    bool isOpen() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::imgui

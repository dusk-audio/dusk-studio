#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>

#include "../session/Session.h"

namespace duskstudio
{
// Bulletproof cross-OS cursor for the edit-mode tools (Grab / Cut /
// Draw / Range / Grid). Bypasses the platform cursor pipeline
// entirely — paints the glyph as a transparent JUCE Component
// overlay polled from juce::Desktop::getMousePosition(). Works
// identically on macOS / Windows / Linux X11 / Wayland / XWayland;
// immune to native cursor theme issues, peer-type dispatch bugs in
// the JUCE-wayland fork, and compositor cursor overrides.
//
// Usage:
//   1. Construct one as a child of MainComponent (or any top-level
//      component visible above the editor surfaces).
//   2. setAlwaysOnTop (true), setInterceptsMouseClicks (false, false).
//   3. setResolver — a callback that, given the GLOBAL mouse position,
//      returns the EditMode to draw or std::nullopt to draw nothing
//      (mouse outside an editor surface that supports the tool).
//   4. The overlay polls the mouse at 60 Hz and repaints on motion.
//
// Editor surfaces that want the overlay glyph instead of the native
// cursor MUST also call setMouseCursor (juce::MouseCursor::NoCursor)
// on their content area while in Grab / Cut / Draw mode, otherwise
// the native cursor will show through the transparent overlay and
// the user sees both at once.
class CursorOverlay final : public juce::Component,
                              private juce::Timer
{
public:
    // Returns the EditMode glyph to paint at the given GLOBAL screen
    // position, or std::nullopt to paint nothing (no overlay glyph;
    // native cursor takes over).
    using Resolver = std::function<std::optional<EditMode> (juce::Point<int> globalMousePos)>;

    CursorOverlay();
    ~CursorOverlay() override = default;

    void setResolver (Resolver r);

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    Resolver         resolver;
    juce::Point<int> lastLocal   { -1000, -1000 };
    bool             lastPainting = false;
    EditMode         lastMode     = EditMode::Grab;
};
} // namespace duskstudio

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../session/Session.h"

namespace duskstudio
{
// Bulletproof cross-OS cursor for the edit-mode tools (Grab / Cut /
// Draw / Range / Grid). Bypasses the platform cursor pipeline
// entirely - paints the glyph as a transparent JUCE Component
// overlay polled from juce::Desktop::getMousePosition(). Works
// identically on macOS / Windows / Linux X11 / Wayland / XWayland;
// immune to native cursor theme issues, peer-type dispatch bugs in
// the JUCE-wayland fork, and compositor cursor overrides.
//
// Usage:
//   1. Construct one as a child of MainComponent (or any top-level
//      component visible above the editor surfaces).
//   2. setAlwaysOnTop (true), setInterceptsMouseClicks (false, false).
//   3. Editor surfaces push their local mouse position in via
//      setMousePosition (source, localPoint, EditMode, ...) from their
//      own mouseMove / mouseDrag; the overlay converts to its own coords
//      through the JUCE component tree (NOT screen space - Wayland's
//      screen coords are unreliable) and draws that EditMode's glyph, or
//      nothing after clearMousePosition() on mouseExit.
//
// Editor surfaces that want the overlay glyph instead of the native
// cursor MUST also call setMouseCursor (juce::MouseCursor::NoCursor)
// on their content area while in Grab / Cut / Draw mode, otherwise
// the native cursor will show through the transparent overlay and
// the user sees both at once.
class CursorOverlay final : public juce::Component
{
public:
    CursorOverlay();
    // Not defaulted: if we're destroyed while hiding the native cursor
    // (lastPainting), the hide must be balanced or the OS cursor stays gone.
    ~CursorOverlay() override;

    void paint (juce::Graphics&) override;

    // Editors call this from their own mouseMove with (e.x, e.y) in
    // their own local coords + the wanted glyph mode. The overlay
    // converts to its own local via JUCE's tree-based getLocalPoint
    // (component-to-component, NOT screen-based - Wayland has broken
    // screen coords). Call with EditMode::Grab/Cut/Draw when over a
    // content area; call clearMousePosition() on mouseExit / when the
    // editor's content area shouldn't show a glyph.
    //
    // cutLineYInSource: in Cut mode, the Y range in `source`-local
    // coords across which a vertical dashed cut line should be drawn
    // (paired with the half-scissor variant). Pass an empty range
    // (default) to keep the cursor on the full-scissor glyph with no
    // line.
    void setMousePosition (juce::Component& source,
                            juce::Point<int> localInSource,
                            EditMode mode,
                            juce::Range<int> cutLineYInSource = {});
    void clearMousePosition();

private:
    void setNativeCursorVisible (bool visible);
    juce::Rectangle<int> glyphDirtyRect (juce::Point<int> local,
                                          juce::Range<int> cutLineY) const;

    juce::Point<int> lastLocal   { -1000, -1000 };
    bool             lastPainting = false;
    EditMode         lastMode     = EditMode::Grab;
    juce::Range<int> lastCutLineY { };          // empty = no dashed line, full scissor
};
} // namespace duskstudio

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../session/Session.h"

namespace duskstudio
{
juce::MouseCursor cursorForEditMode (EditMode m);

// True invisible image cursor — 2x2 transparent ARGB image cursor that
// goes through the SAME image-cursor pipeline as our scissors / hand /
// pencil custom cursors. JUCE's MouseCursor::NoCursor is a special-case
// enum that hits a different (broken-on-some-Linux-WMs) code path; use
// this when you need the native cursor invisible AND the image-cursor
// path has been verified working.
juce::MouseCursor invisibleCursor();

// Direct glyph paint helpers — used both by the native MouseCursor
// image-builders below AND by the CursorOverlay component that bypasses
// the platform cursor pipeline. Each paints the glyph centred at (cx,
// cy) in the supplied Graphics context, sized to fit a 24×24 cursor box.
// Hotspot semantics mirror the cursor-image builders.
void paintHandGlyph     (juce::Graphics& g, float cx, float cy);
void paintScissorsGlyph (juce::Graphics& g, float cx, float cy);
void paintPencilGlyph   (juce::Graphics& g, float cx, float cy);


// JUCE's default-constructed MouseCursor compares as NormalCursor (NOT
// ParentCursor), so a parent's setMouseCursor is silently shadowed by
// every label / static child the mouse hovers. Walk the tree and flip
// any descendant whose cursor is still the default NormalCursor over
// to ParentCursor so the parent's stored cursor is what actually
// renders. Children that explicitly chose a cursor (resize handles,
// fader caps, learn buttons, etc.) are left alone.
void inheritCursorOnDescendants (juce::Component& root);
} // namespace duskstudio

#include "EditCursors.h"

namespace duskstudio
{
// ── Public glyph paint helpers ──────────────────────────────────────
// Used by makeXxxCursor (image cursor builders) AND by CursorOverlay
// (direct paint, bypassing the platform cursor pipeline). Each helper
// expects (cx, cy) to be the cursor hotspot in the supplied Graphics
// context; the glyph's hotspot location relative to the bounding box
// matches the corresponding makeXxxCursor's `juce::MouseCursor(img,
// hotX, hotY)` so the overlay glyph lands on the same pixel as the
// native cursor would.
void paintScissorsGlyph (juce::Graphics& g, float cx, float cy)
{
    // makeScissorsCursor hotspot = (20, 5). Translate so the path's
    // (20, 5) lands at (cx, cy).
    const float ox = cx - 20.0f;
    const float oy = cy - 5.0f;

    juce::Path blades;
    blades.startNewSubPath (ox + 4.0f,  oy + 19.0f); blades.lineTo (ox + 20.0f, oy + 5.0f);
    blades.startNewSubPath (ox + 11.0f, oy + 19.0f); blades.lineTo (ox + 20.0f, oy + 13.0f);
    juce::Path loops;
    loops.addEllipse (ox + 2.0f, oy + 17.0f, 5.0f, 5.0f);
    loops.addEllipse (ox + 9.0f, oy + 17.0f, 5.0f, 5.0f);

    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.strokePath (blades, juce::PathStrokeType (3.4f));
    g.strokePath (loops,  juce::PathStrokeType (3.4f));
    g.setColour (juce::Colours::white);
    g.strokePath (blades, juce::PathStrokeType (1.6f));
    g.strokePath (loops,  juce::PathStrokeType (1.6f));
}

void paintPencilGlyph (juce::Graphics& g, float cx, float cy)
{
    // makePencilCursor hotspot = (3, 21).
    const float ox = cx - 3.0f;
    const float oy = cy - 21.0f;

    juce::Path body;
    body.addQuadrilateral (ox + 3.0f,  oy + 21.0f,
                            ox + 7.0f,  oy + 16.0f,
                            ox + 20.0f, oy + 3.0f,
                            ox + 16.0f, oy + 8.0f);
    juce::Path tip;
    tip.addTriangle (ox + 3.0f, oy + 21.0f,
                      ox + 7.0f, oy + 16.0f,
                      ox + 5.5f, oy + 18.5f);

    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.strokePath (body, juce::PathStrokeType (3.0f));
    g.setColour (juce::Colours::white);
    g.fillPath (body);
    g.setColour (juce::Colours::black);
    g.fillPath (tip);
}

void paintHandGlyph (juce::Graphics& g, float cx, float cy)
{
    // makeHandCursor hotspot = (12, 12) — palm centre.
    const float ox = cx - 12.0f;
    const float oy = cy - 12.0f;

    juce::Path palm;
    palm.addRoundedRectangle (ox + 6.0f, oy + 10.0f, 12.0f, 11.0f, 2.0f);
    juce::Path fingers;
    fingers.addRoundedRectangle (ox + 7.0f,  oy + 4.0f, 2.4f, 8.0f, 1.2f);
    fingers.addRoundedRectangle (ox + 10.0f, oy + 3.0f, 2.4f, 9.0f, 1.2f);
    fingers.addRoundedRectangle (ox + 13.0f, oy + 4.0f, 2.4f, 8.0f, 1.2f);
    fingers.addRoundedRectangle (ox + 16.0f, oy + 6.0f, 2.4f, 6.0f, 1.2f);
    juce::Path thumb;
    thumb.addRoundedRectangle (ox + 18.0f, oy + 12.0f, 3.0f, 6.0f, 1.4f);

    g.setColour (juce::Colours::black.withAlpha (0.75f));
    g.strokePath (palm,    juce::PathStrokeType (3.4f));
    g.strokePath (fingers, juce::PathStrokeType (3.4f));
    g.strokePath (thumb,   juce::PathStrokeType (3.4f));
    g.setColour (juce::Colours::white);
    g.fillPath (palm);
    g.fillPath (fingers);
    g.fillPath (thumb);
}

namespace
{
juce::Image drawCursorImage (float hotX, float hotY,
                              void (*paintFn) (juce::Graphics&, float, float))
{
    constexpr int kSz = 24;
    juce::Image img (juce::Image::ARGB, kSz, kSz, true);
    juce::Graphics g (img);
    paintFn (g, hotX, hotY);
    return img;
}

juce::MouseCursor makeScissorsCursor()
{
    return juce::MouseCursor (drawCursorImage (20.0f, 5.0f, &paintScissorsGlyph), 20, 5);
}

juce::MouseCursor makePencilCursor()
{
    return juce::MouseCursor (drawCursorImage (3.0f, 21.0f, &paintPencilGlyph), 3, 21);
}

juce::MouseCursor makeHandCursor()
{
    return juce::MouseCursor (drawCursorImage (12.0f, 12.0f, &paintHandGlyph), 12, 12);
}
} // namespace

void inheritCursorOnDescendants (juce::Component& root)
{
    for (auto* child : root.getChildren())
    {
        if (child == nullptr) continue;
        // JUCE's default-constructed MouseCursor compares as NormalCursor
        // (null cursorHandle). Replace it with ParentCursor so the
        // editor's setMouseCursor reaches through. Children that already
        // picked a non-default cursor (Slider rotary, MIDI Learn buttons,
        // fade resize handles, text-input labels) keep theirs.
        if (child->getMouseCursor() == juce::MouseCursor::NormalCursor)
            child->setMouseCursor (juce::MouseCursor::ParentCursor);

        inheritCursorOnDescendants (*child);
    }
}

juce::MouseCursor cursorForEditMode (EditMode m)
{
    static const juce::MouseCursor hand     = makeHandCursor();
    static const juce::MouseCursor scissors = makeScissorsCursor();
    static const juce::MouseCursor pencil   = makePencilCursor();
    switch (m)
    {
        case EditMode::Grab:  return hand;
        case EditMode::Range: return juce::MouseCursor::IBeamCursor;
        case EditMode::Cut:   return scissors;
        case EditMode::Grid:  return juce::MouseCursor::CrosshairCursor;
        case EditMode::Draw:  return pencil;
    }
    return juce::MouseCursor::NormalCursor;
}
} // namespace duskstudio

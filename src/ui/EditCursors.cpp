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
    // Full open-X with ring-loop handles at the bottom corners.
    // Sized so the full glyph (path + halo stroke) fits strictly
    // inside a 24×24 box centred on the hotspot at (cx, cy). a +
    // loopR + halo half-stroke must stay ≤ 12 (the half-extent of
    // 24×24).
    constexpr float a       = 8.0f;
    constexpr float loopR   = 2.5f;
    juce::Path blades;
    blades.startNewSubPath (cx - a, cy - a); blades.lineTo (cx + a, cy + a);
    blades.startNewSubPath (cx + a, cy - a); blades.lineTo (cx - a, cy + a);

    juce::Path loops;
    loops.addEllipse (cx - a - loopR, cy + a - loopR, loopR * 2.0f, loopR * 2.0f);
    loops.addEllipse (cx + a - loopR, cy + a - loopR, loopR * 2.0f, loopR * 2.0f);

    const auto bladeHalo  = juce::PathStrokeType (2.6f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded);
    const auto bladeInner = juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded);
    const auto loopHalo   = juce::PathStrokeType (2.6f);
    const auto loopInner  = juce::PathStrokeType (1.4f);

    g.setColour (juce::Colours::black.withAlpha (0.85f));
    g.strokePath (blades, bladeHalo);
    g.strokePath (loops,  loopHalo);

    g.setColour (juce::Colours::white);
    g.strokePath (blades, bladeInner);
    g.strokePath (loops,  loopInner);
}

void paintPencilGlyph (juce::Graphics& g, float cx, float cy)
{
    // Pro Tools-style pencil: a slim rectangular body rotated ~45°
    // (eraser-end at upper-right, lead-end at lower-left), with a
    // perpendicular ferrule band near the eraser and a darker
    // triangular tip at the lead end. Strictly fits inside a 24×24
    // box with the lead tip at (cx, cy) so the glyph never extends
    // past the cursor's canvas.
    //
    // Hotspot = the lead tip ⇒ position the glyph so its (3, 21) grid
    // coordinate lands at (cx, cy). kSize ≤ 24 keeps the (20, 5)
    // eraser corner within (cx + 17, cy - 16) of the hotspot — under
    // the 24-px canvas budget once the 1.6 px halo is included.
    constexpr float kGrid = 24.0f;
    constexpr float kSize = 22.0f;
    const float ox = cx - (3.0f / kGrid) * kSize;
    const float oy = cy - (21.0f / kGrid) * kSize;
    auto sx = [&] (float u) { return ox + (u / kGrid) * kSize; };
    auto sy = [&] (float u) { return oy + (u / kGrid) * kSize; };

    // Body quadrilateral. Vertex order traces:
    //   tip-bottom (3,21) → tip-top (7,16) → eraser-top (20,5)
    //   → eraser-bottom (17,8). Closed quad = full pencil silhouette.
    juce::Path body;
    body.addQuadrilateral (sx (3.0f),  sy (21.0f),
                            sx (7.0f),  sy (16.0f),
                            sx (20.0f), sy ( 5.0f),
                            sx (17.0f), sy ( 8.0f));

    // Filled triangle at the lead end (sharpened tip). Same colour
    // as the body so the whole pencil reads as a single white
    // silhouette with a black halo for contrast.
    juce::Path tip;
    tip.addTriangle (sx (3.0f),  sy (21.0f),
                      sx (7.0f),  sy (16.0f),
                      sx (5.5f), sy (18.5f));

    const auto bodyHaloStroke = juce::PathStrokeType (2.4f,
                                                        juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded);

    // Halo: black stroke around the body silhouette so the all-white
    // pencil reads on any background.
    g.setColour (juce::Colours::black.withAlpha (0.85f));
    g.strokePath (body, bodyHaloStroke);

    // All-white pencil: body + tip in the same colour. Halo gives
    // the shape; the lead tip stays visible as a slight protrusion
    // beyond the quad's tip corner.
    g.setColour (juce::Colours::white);
    g.fillPath (body);
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

juce::MouseCursor invisibleCursor()
{
    static const juce::MouseCursor c = [] {
        // 2x2 fully-transparent ARGB image. Goes through the same X11
        // XCreatePixmapCursor / NSCursor with empty image / Win32
        // CreateCursor with empty AND mask path that all our custom
        // image cursors use — so it works wherever the image-cursor
        // path works (i.e. anywhere our scissors/hand cursors render).
        juce::Image img (juce::Image::ARGB, 2, 2, true);
        return juce::MouseCursor (img, 0, 0);
    }();
    return c;
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

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
    // Pencil pointing down-left, eraser up-right, the barrel at ~45°. A white
    // silhouette with a black halo (matching the hand / scissors glyphs), with
    // the lead tip and the wood-collar / ferrule section lines drawn in black
    // so it reads as a pencil rather than a plain stick. Built along a single
    // axis from the lead tip (the hotspot, at cx,cy) to the eraser.
    constexpr float kGrid = 24.0f;
    constexpr float kSize = 34.0f;
    // Anchor the lead tip (grid 4,20) on the hotspot (cx,cy).
    const juce::Point<float> tip { 4.0f, 20.0f };
    const float ox = cx - (tip.x / kGrid) * kSize;
    const float oy = cy - (tip.y / kGrid) * kSize;
    auto P = [&] (float gx, float gy)
        { return juce::Point<float> (ox + (gx / kGrid) * kSize, oy + (gy / kGrid) * kSize); };

    // Axis param t = grid units from the tip toward the eraser (45° up-right);
    // half-width offsets run along the perpendicular (down-right).
    constexpr float kInv = 0.70710678f;
    auto axis = [&] (float t) { return juce::Point<float> (tip.x + kInv * t, tip.y - kInv * t); };
    auto eA = [&] (float t, float w) { auto c = axis (t); return P (c.x - kInv * w, c.y - kInv * w); };
    auto eB = [&] (float t, float w) { auto c = axis (t); return P (c.x + kInv * w, c.y + kInv * w); };
    auto quad = [&] (float t0, float w0, float t1, float w1)
    {
        juce::Path p;
        p.startNewSubPath (eA (t0, w0));
        p.lineTo (eB (t0, w0));
        p.lineTo (eB (t1, w1));
        p.lineTo (eA (t1, w1));
        p.closeSubPath();
        return p;
    };

    constexpr float W  = 3.0f;   // barrel half-width
    constexpr float wG = 1.2f;   // graphite base half-width
    // Section boundaries (grid units from the tip).
    constexpr float tGraphite = 2.4f;
    constexpr float tWood     = 5.8f;
    constexpr float tBody     = 16.0f;
    constexpr float tFerrule  = 18.6f;
    constexpr float tEnd      = 21.0f;

    const auto apex = P (tip.x, tip.y);

    // Outer silhouette (apex → wood widen → straight barrel → eraser end and
    // back) for the halo stroke.
    juce::Path sil;
    sil.startNewSubPath (apex);
    sil.lineTo (eA (tWood, W));
    sil.lineTo (eA (tEnd,  W));
    sil.lineTo (eB (tEnd,  W));
    sil.lineTo (eB (tWood, W));
    sil.closeSubPath();

    // Black halo + white body, matching the hand / scissors glyphs. The
    // pencil sections are drawn as black detail strokes on the white
    // silhouette rather than colour fills.
    g.setColour (juce::Colours::black.withAlpha (0.85f));
    g.strokePath (sil, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    g.setColour (juce::Colours::white);
    g.fillPath (sil);

    // Black lead point at the tip.
    juce::Path graphite;
    graphite.startNewSubPath (apex);
    graphite.lineTo (eA (tGraphite, wG));
    graphite.lineTo (eB (tGraphite, wG));
    graphite.closeSubPath();
    g.setColour (juce::Colours::black.withAlpha (0.85f));
    g.fillPath (graphite);

    // Section divider lines (wood collar, the two ferrule-band edges) so the
    // pencil reads as a pencil without colour.
    auto cross = [&] (float t)
    {
        const auto a = eA (t, W), b = eB (t, W);
        g.drawLine (a.getX(), a.getY(), b.getX(), b.getY(), 1.3f);
    };
    cross (tWood);
    cross (tBody);
    cross (tFerrule);
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
                              void (*paintFn) (juce::Graphics&, float, float),
                              int kSz = 24)
{
    juce::Image img (juce::Image::ARGB, kSz, kSz, true);
    juce::Graphics g (img);
    paintFn (g, hotX, hotY);
    return img;
}

juce::MouseCursor makeScissorsCursor()
{
    // Centre the glyph in the 24x24 image: paintScissorsGlyph draws +/-12 px
    // around the hotspot (a + loopR + halo half-stroke = 11.8), so (20, 5)
    // clipped the blades top/right. (12, 12) also puts the cut hotspot on the
    // blade crossing and matches CursorOverlay's centred draw.
    return juce::MouseCursor (drawCursorImage (12.0f, 12.0f, &paintScissorsGlyph), 12, 12);
}

juce::MouseCursor makePencilCursor()
{
    // Lead tip is the hotspot, near the lower-left; the body extends up-right.
    // 36-px canvas fits the enlarged glyph (kSize 34) + halo without clipping.
    return juce::MouseCursor (drawCursorImage (5.0f, 30.0f, &paintPencilGlyph, 36), 5, 30);
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

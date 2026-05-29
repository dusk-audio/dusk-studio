#include "EditCursors.h"

namespace duskstudio
{
namespace
{
juce::Image drawCursorGlyph (std::function<void (juce::Graphics&)> paint)
{
    constexpr int kSz = 24;
    juce::Image img (juce::Image::ARGB, kSz, kSz, true);
    juce::Graphics g (img);
    paint (g);
    return img;
}

juce::MouseCursor makeScissorsCursor()
{
    auto img = drawCursorGlyph ([] (juce::Graphics& g)
    {
        juce::Path blades;
        blades.startNewSubPath (4.0f, 19.0f); blades.lineTo (20.0f, 5.0f);
        blades.startNewSubPath (11.0f, 19.0f); blades.lineTo (20.0f, 13.0f);
        juce::Path loops;
        loops.addEllipse (2.0f, 17.0f, 5.0f, 5.0f);
        loops.addEllipse (9.0f, 17.0f, 5.0f, 5.0f);

        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.strokePath (blades, juce::PathStrokeType (3.4f));
        g.strokePath (loops,  juce::PathStrokeType (3.4f));
        g.setColour (juce::Colours::white);
        g.strokePath (blades, juce::PathStrokeType (1.6f));
        g.strokePath (loops,  juce::PathStrokeType (1.6f));
    });
    return juce::MouseCursor (img, 20, 5);
}

juce::MouseCursor makePencilCursor()
{
    auto img = drawCursorGlyph ([] (juce::Graphics& g)
    {
        juce::Path body;
        body.addQuadrilateral (3.0f, 21.0f, 7.0f, 16.0f, 20.0f, 3.0f, 16.0f, 8.0f);
        juce::Path tip;
        tip.addTriangle (3.0f, 21.0f, 7.0f, 16.0f, 5.5f, 18.5f);

        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.strokePath (body, juce::PathStrokeType (3.0f));
        g.setColour (juce::Colours::white);
        g.fillPath (body);
        g.setColour (juce::Colours::black);
        g.fillPath (tip);
    });
    return juce::MouseCursor (img, 3, 21);
}

juce::MouseCursor makeHandCursor()
{
    // Custom open-hand glyph for Grab mode. JUCE's built-in
    // PointingHandCursor / DraggingHandCursor render inconsistently
    // across Linux X11 cursor themes — some show a finger, some show
    // the fleur 4-arrow, some show nothing. A custom Image cursor is
    // theme-immune.
    auto img = drawCursorGlyph ([] (juce::Graphics& g)
    {
        // 4 fingers + thumb on the left. Hotspot = palm centre.
        juce::Path palm;
        palm.addRoundedRectangle (6.0f, 10.0f, 12.0f, 11.0f, 2.0f);
        juce::Path fingers;
        // Index, middle, ring, pinky — 4 vertical caps above the palm.
        fingers.addRoundedRectangle ( 7.0f, 4.0f, 2.4f, 8.0f, 1.2f);
        fingers.addRoundedRectangle (10.0f, 3.0f, 2.4f, 9.0f, 1.2f);
        fingers.addRoundedRectangle (13.0f, 4.0f, 2.4f, 8.0f, 1.2f);
        fingers.addRoundedRectangle (16.0f, 6.0f, 2.4f, 6.0f, 1.2f);
        // Thumb out to the right.
        juce::Path thumb;
        thumb.addRoundedRectangle (18.0f, 12.0f, 3.0f, 6.0f, 1.4f);

        // Dark outline first so the white glyph reads against any
        // background colour.
        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.strokePath (palm,    juce::PathStrokeType (3.4f));
        g.strokePath (fingers, juce::PathStrokeType (3.4f));
        g.strokePath (thumb,   juce::PathStrokeType (3.4f));
        g.setColour (juce::Colours::white);
        g.fillPath (palm);
        g.fillPath (fingers);
        g.fillPath (thumb);
    });
    return juce::MouseCursor (img, 12, 12);   // hotspot ≈ palm centre
}
} // namespace

juce::MouseCursor cursorForEditMode (EditMode m)
{
    static const juce::MouseCursor hand     = makeHandCursor();
    static const juce::MouseCursor scissors = makeScissorsCursor();
    static const juce::MouseCursor pencil   = makePencilCursor();
    switch (m)
    {
        // Custom hand image — theme-immune across macOS / Windows /
        // Linux X11. JUCE's built-in PointingHand / DraggingHand
        // cursors render as fleur (4-arrow) or even plain arrow under
        // some Linux X11 cursor themes.
        case EditMode::Grab:  return hand;
        case EditMode::Range: return juce::MouseCursor::IBeamCursor;
        case EditMode::Cut:   return scissors;
        case EditMode::Grid:  return juce::MouseCursor::CrosshairCursor;
        case EditMode::Draw:  return pencil;
    }
    return juce::MouseCursor::NormalCursor;
}
} // namespace duskstudio

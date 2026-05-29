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
} // namespace

juce::MouseCursor cursorForEditMode (EditMode m)
{
    static const juce::MouseCursor scissors = makeScissorsCursor();
    static const juce::MouseCursor pencil   = makePencilCursor();
    switch (m)
    {
        case EditMode::Grab:  return juce::MouseCursor::DraggingHandCursor;
        case EditMode::Range: return juce::MouseCursor::IBeamCursor;
        case EditMode::Cut:   return scissors;
        case EditMode::Grid:  return juce::MouseCursor::CrosshairCursor;
        case EditMode::Draw:  return pencil;
    }
    return juce::MouseCursor::NormalCursor;
}
} // namespace duskstudio

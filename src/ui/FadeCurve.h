#pragma once

#include <juce_graphics/juce_graphics.h>
#include "../session/Session.h"
#include <algorithm>
#include <cmath>

namespace duskstudio::fadeviz
{
// Build a fade curve as a Path across `zone`, sampling applyFadeShape so
// the painted curve matches what PlaybackEngine renders.
//   rising  = fade-in : bottom-left (gain 0) -> top-right (gain 1)
//   !rising = fade-out: top-left   (gain 1) -> bottom-right (gain 0)
// One vertex per horizontal pixel; clamps to >= 1 px wide.
inline juce::Path buildFadeCurve (juce::Rectangle<float> zone,
                                   FadeShape shape, bool rising)
{
    juce::Path p;
    const int span = std::max (1, (int) std::round (zone.getWidth()));
    for (int i = 0; i <= span; ++i)
    {
        const float t    = (float) i / (float) span;
        const float gain = applyFadeShape (rising ? t : 1.0f - t, shape);
        const float x    = zone.getX() + t * zone.getWidth();
        const float y    = zone.getBottom() - gain * zone.getHeight();
        if (i == 0) p.startNewSubPath (x, y);
        else        p.lineTo          (x, y);
    }
    return p;
}

// Render a crossfade "X" over `zone`: a translucent fill behind the two
// crossing curves (outgoing region's fade-out + incoming region's
// fade-in). Where the curves meet they form the X.
inline void drawCrossfade (juce::Graphics& g, juce::Rectangle<float> zone,
                            FadeShape outShape, FadeShape inShape,
                            juce::Colour curveColour, juce::Colour fillColour,
                            float strokeWidth = 1.4f)
{
    if (zone.getWidth() < 1.0f || zone.getHeight() < 1.0f) return;
    g.setColour (fillColour);
    g.fillRect (zone);
    g.setColour (curveColour);
    g.strokePath (buildFadeCurve (zone, outShape, /*rising*/ false),
                   juce::PathStrokeType (strokeWidth));
    g.strokePath (buildFadeCurve (zone, inShape, /*rising*/ true),
                   juce::PathStrokeType (strokeWidth));
}
} // namespace duskstudio::fadeviz

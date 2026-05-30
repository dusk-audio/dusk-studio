#include "CursorOverlay.h"
#include "EditCursors.h"

namespace duskstudio
{
CursorOverlay::CursorOverlay()
{
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    setAlwaysOnTop (true);
    startTimerHz (60);
}

void CursorOverlay::setResolver (Resolver r)
{
    resolver = std::move (r);
}

void CursorOverlay::timerCallback()
{
    if (! isShowing())
    {
        if (lastPainting)
        {
            lastPainting = false;
            repaint();
        }
        return;
    }

    const auto globalPos = juce::Desktop::getMousePosition();
    const auto localPos  = getLocalPoint (nullptr, globalPos);

    std::optional<EditMode> wanted;
    if (resolver) wanted = resolver (globalPos);

    const bool wantPaint = wanted.has_value();
    const EditMode mode  = wantPaint ? *wanted : EditMode::Grab;

    // Repaint only when something the painter cares about changed —
    // position, mode, or paint-vs-no-paint state. 60 Hz polling is
    // cheap; per-frame repaint at high res can hit retained-mode
    // CPU on systems without GPU-accelerated invalidate.
    if (wantPaint != lastPainting
        || (wantPaint && (localPos != lastLocal || mode != lastMode)))
    {
        // When transitioning from painting → not painting (or moving),
        // invalidate the OLD glyph rect so it gets cleared.
        if (lastPainting)
            repaint (juce::Rectangle<int> (lastLocal.x - 16, lastLocal.y - 16,
                                              32, 32));
        lastLocal    = localPos;
        lastMode     = mode;
        lastPainting = wantPaint;
        if (wantPaint)
            repaint (juce::Rectangle<int> (localPos.x - 16, localPos.y - 16,
                                              32, 32));
    }
}

void CursorOverlay::paint (juce::Graphics& g)
{
    if (! lastPainting) return;

    const float cx = (float) lastLocal.x;
    const float cy = (float) lastLocal.y;

    switch (lastMode)
    {
        case EditMode::Grab:  paintHandGlyph     (g, cx, cy); break;
        case EditMode::Cut:   paintScissorsGlyph (g, cx, cy); break;
        case EditMode::Draw:  paintPencilGlyph   (g, cx, cy); break;
        case EditMode::Range:
        case EditMode::Grid:
        default:
            // Range = I-beam, Grid = crosshair — those are standard
            // OS cursors and don't need a custom-paint fallback. The
            // resolver returns nullopt for them so this branch is
            // unreachable in practice; kept as a clean default.
            break;
    }
}
} // namespace duskstudio

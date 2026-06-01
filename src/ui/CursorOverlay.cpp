#include "CursorOverlay.h"
#include "EditCursors.h"
#include "PlatformWindowing.h"

namespace duskstudio
{
CursorOverlay::CursorOverlay()
{
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    setAlwaysOnTop (true);
    setWantsKeyboardFocus (false);
}

CursorOverlay::~CursorOverlay()
{
    // Balance an outstanding hide so the native cursor isn't left invisible
    // if the overlay is torn down mid-paint (e.g. editor modal closes while
    // the Grab/Cut/Draw glyph is showing).
    if (lastPainting)
        setNativeCursorVisible (true);
}

void CursorOverlay::setMousePosition (juce::Component& source,
                                        juce::Point<int> localInSource,
                                        EditMode mode,
                                        juce::Range<int> cutLineYInSource)
{
    const auto myLocal = getLocalPoint (&source, localInSource);

    // Convert the cut-line Y range from source-local to overlay-local
    // by mapping the two endpoints individually through the JUCE tree.
    juce::Range<int> myCutLine;
    if (! cutLineYInSource.isEmpty())
    {
        const int y0 = getLocalPoint (&source,
                                        juce::Point<int> (localInSource.x,
                                                            cutLineYInSource.getStart())).y;
        const int y1 = getLocalPoint (&source,
                                        juce::Point<int> (localInSource.x,
                                                            cutLineYInSource.getEnd())).y;
        myCutLine = juce::Range<int> (juce::jmin (y0, y1), juce::jmax (y0, y1));
    }

    // Only Grab / Cut / Draw paint a glyph at the cursor. Range / Grid draw
    // nothing (see paint()), so suppressing the OS cursor for them would just
    // leave an invisible pointer with no replacement. Hide only for painting
    // modes, and restore the native cursor when transitioning out of one.
    const bool paints = (mode == EditMode::Grab || mode == EditMode::Cut
                          || mode == EditMode::Draw);

    // Nothing to redraw when position / mode / cut-line are unchanged AND the
    // paint-state matches — covers both "still painting the same spot" and a
    // non-painting mode (Range/Grid) parked in place (no redundant repaint).
    if (myLocal == lastLocal && mode == lastMode
        && myCutLine == lastCutLineY && lastPainting == paints)
        return;

    const bool wasPainting = lastPainting;
    lastLocal    = myLocal;
    lastMode     = mode;
    lastCutLineY = myCutLine;
    lastPainting = paints;

    if (paints && ! wasPainting)       setNativeCursorVisible (false);
    else if (! paints && wasPainting)  setNativeCursorVisible (true);

    // Full repaint each move so the old glyph position is properly cleared by
    // JUCE's parent-repaint-under pass — partial-rect repaint was leaving
    // trails. Skip entirely when neither the old nor the new mode paints a
    // glyph (Range/Grid moving): paint() is a no-op there, so a repaint would
    // just invalidate the full surface for nothing.
    if (paints || wasPainting)
        repaint();
}

void CursorOverlay::clearMousePosition()
{
    if (! lastPainting) return;
    lastPainting = false;
    setNativeCursorVisible (true);
    repaint();
}

void CursorOverlay::setNativeCursorVisible (bool visible)
{
    if (auto* peer = getPeer())
        platform::setNativeCursorVisibleOnPeer (*peer, visible);
}

void CursorOverlay::paint (juce::Graphics& g)
{
    if (! lastPainting) return;

    const float cx = (float) lastLocal.x;
    const float cy = (float) lastLocal.y;

    switch (lastMode)
    {
        case EditMode::Grab:  paintHandGlyph (g, cx, cy); break;
        case EditMode::Cut:
            // Dashed vertical cut line FIRST (behind the scissor) so
            // the glyph stays the unambiguous "what tool am I" marker
            // at the cursor while the line communicates where the
            // split will land. Range empty = no line, plain scissor.
            if (! lastCutLineY.isEmpty())
            {
                constexpr float kDash = 3.0f;
                constexpr float kGap  = 3.0f;
                const float y0 = (float) lastCutLineY.getStart();
                const float y1 = (float) lastCutLineY.getEnd();
                auto drawDashes = [&] (juce::Colour col, float thickness)
                {
                    g.setColour (col);
                    for (float y = y0; y < y1; y += kDash + kGap)
                    {
                        const float yEnd = juce::jmin (y + kDash, y1);
                        g.drawLine (cx, y, cx, yEnd, thickness);
                    }
                };
                drawDashes (juce::Colours::black.withAlpha (0.85f), 2.2f);
                drawDashes (juce::Colours::white,                   1.2f);
            }
            paintScissorsGlyph (g, cx, cy);
            break;
        case EditMode::Draw:  paintPencilGlyph (g, cx, cy); break;
        case EditMode::Range:
        case EditMode::Grid:
        default: break;
    }
}
} // namespace duskstudio

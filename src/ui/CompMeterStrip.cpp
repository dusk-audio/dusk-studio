#include "CompMeterStrip.h"
#include "FocalLookAndFeel.h"

namespace focal
{
CompMeterStrip::CompMeterStrip (Track& t) : track (t)
{
    setOpaque (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    startTimerHz (30);
}

CompMeterStrip::~CompMeterStrip() = default;

float CompMeterStrip::dbToFrac (float db) const noexcept
{
    return juce::jlimit (0.0f, 1.0f, (db - kFloorDb) / (kCeilingDb - kFloorDb));
}

float CompMeterStrip::fracToDb (float frac) const noexcept
{
    return juce::jlimit (kFloorDb, kCeilingDb,
                          kFloorDb + frac * (kCeilingDb - kFloorDb));
}

float CompMeterStrip::yForDb (float db, juce::Rectangle<float> area) const noexcept
{
    const float frac = dbToFrac (db);
    return area.getBottom() - 2.0f - frac * (area.getHeight() - 4.0f);
}

float CompMeterStrip::dbForY (int y, juce::Rectangle<float> area) const noexcept
{
    const float relative = (area.getBottom() - 2.0f - (float) y) / (area.getHeight() - 4.0f);
    return fracToDb (relative);
}

void CompMeterStrip::resized()
{
    auto b = getLocalBounds().toFloat().reduced (1.0f);
    const float W = b.getWidth();
    // Proportional layout so the strip looks right both as a narrow inline
    // meter (~28-32 px wide) and as a wider popup meter (~80 px). Handle area
    // is just enough for the threshold triangle; the two bars split the rest.
    const float handleW = juce::jlimit (5.0f, 12.0f, W * 0.18f);
    const float gap     = juce::jmax (1.0f, W * 0.04f);
    const float barW    = juce::jmax (4.0f, (W - handleW - gap * 2.0f) / 2.0f);
    handleArea   = b.removeFromLeft (handleW);
    b.removeFromLeft (gap);
    inputBarArea = b.removeFromLeft (barW);
    b.removeFromLeft (gap);
    grBarArea    = b.removeFromLeft (barW);
}

void CompMeterStrip::timerCallback()
{
    const float inputDb = track.meterInputDb.load (std::memory_order_relaxed);
    if (inputDb > displayedInputDb)
        displayedInputDb = inputDb;
    else
        displayedInputDb += (inputDb - displayedInputDb) * 0.15f;

    if (inputDb >= inputPeakHoldDb)
    {
        inputPeakHoldDb = inputDb;
        inputPeakHoldFrames = 18;  // ~600 ms at 30 Hz
    }
    else if (inputPeakHoldFrames > 0)
    {
        --inputPeakHoldFrames;
    }
    else
    {
        inputPeakHoldDb = juce::jmax (kFloorDb, inputPeakHoldDb - 1.5f);
    }

    const float gr = track.meterGrDb.load (std::memory_order_relaxed);
    if (gr < displayedGrDb)
        displayedGrDb = gr;
    else
        displayedGrDb += (gr - displayedGrDb) * 0.18f;

    repaint();
}

namespace
{
// Renders one segmented-LED bar - dark background, gradient fill, and thin
// black gridlines that give the stacked-LED look from hardware level meters.
void drawSegmentedBar (juce::Graphics& g,
                        juce::Rectangle<float> bar, float frac,
                        juce::Colour topColour, juce::Colour bottomColour,
                        bool fillFromTop)
{
    g.setColour (juce::Colour (0xff060608));
    g.fillRoundedRectangle (bar, 1.5f);

    const float clamped = juce::jlimit (0.0f, 1.0f, frac);
    if (clamped > 0.001f)
    {
        const float fillH = (bar.getHeight() - 2.0f) * clamped;
        const float x = bar.getX() + 1.0f;
        const float w = bar.getWidth() - 2.0f;
        const float y = fillFromTop ? bar.getY() + 1.0f
                                     : bar.getBottom() - 1.0f - fillH;
        juce::ColourGradient grad (topColour, x, bar.getY(),
                                     bottomColour, x, bar.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRect (juce::Rectangle<float> (x, y, w, fillH));
    }

    // Segment dividers - thin black lines every ~3 px give the stacked-LED
    // look. Segment count scales with bar height so small and large bars
    // both look right.
    const int segments = juce::jlimit (8, 30, (int) (bar.getHeight() / 3.5f));
    const float segStep = bar.getHeight() / (float) segments;
    g.setColour (juce::Colour (0xff020203));
    for (int i = 1; i < segments; ++i)
    {
        const float yy = bar.getY() + i * segStep;
        g.fillRect (juce::Rectangle<float> (bar.getX() + 1.0f, yy - 0.4f,
                                              bar.getWidth() - 2.0f, 0.8f));
    }

    // Subtle outline.
    g.setColour (juce::Colour (0xff2a2a30));
    g.drawRoundedRectangle (bar, 1.5f, 0.5f);
}
} // namespace

void CompMeterStrip::paint (juce::Graphics& g)
{
    auto bg = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff141418));
    g.fillRoundedRectangle (bg, 2.0f);
    g.setColour (juce::Colour (0xff2a2a30));
    g.drawRoundedRectangle (bg, 2.0f, 0.6f);

    // Input meter - green at the bottom, yellow mid, red top. Fills upward.
    {
        const float frac = dbToFrac (displayedInputDb);
        drawSegmentedBar (g, inputBarArea, frac,
                          juce::Colour (0xffe05050),    // top (loud)
                          juce::Colour (0xff44d058),    // bottom (quiet)
                          /*fillFromTop=*/false);

        // Peak-hold tick - bright, lingers ~600 ms before falling.
        const float peakFrac = dbToFrac (inputPeakHoldDb);
        if (peakFrac > 0.001f)
        {
            const float y = inputBarArea.getBottom() - 1.0f
                              - peakFrac * (inputBarArea.getHeight() - 2.0f);
            g.setColour (peakFrac > dbToFrac (-3.0f) ? juce::Colour (0xffff8080)
                                                      : juce::Colour (0xfff0f0f0));
            g.fillRect (juce::Rectangle<float> (inputBarArea.getX() + 1.0f, y - 0.5f,
                                                  inputBarArea.getWidth() - 2.0f, 1.4f));
        }
    }

    // GR meter - gold at top tapering to red as compression deepens. Fills
    // downward from top, so the bar "drops" out of the top edge as the comp
    // pulls the signal down. Mirrors how mixbus/Leveler draws GR.
    {
        const float grAbs = juce::jlimit (0.0f, std::abs (kGrFloorDb),
                                            std::abs (displayedGrDb));
        const float frac = grAbs / std::abs (kGrFloorDb);
        drawSegmentedBar (g, grBarArea, frac,
                          juce::Colour (fourKColors::kCompGold).brighter (0.25f),  // top
                          juce::Colour (fourKColors::kHfRed).brighter (0.10f),      // bottom (deep GR)
                          /*fillFromTop=*/true);
    }

    // Threshold marker. Read the per-mode atomic the engine actually uses,
    // then convert back into the unified "threshold dB" axis so the
    // triangle's vertical position reflects the active mode's true threshold.
    // Without this, dragging in Opto/FET wrote to per-mode atomics but the
    // triangle stayed pinned because paint only read compVcaThreshDb.
    //   Opto  - peakRed (0..100 %) -> -60 .. 0 dB
    //   FET   - fet_input drive (0..40 dB) -> -drive
    //   VCA   - compVcaThreshDb directly
    {
        const int mode = juce::jlimit (0, 2,
            track.strip.compMode.load (std::memory_order_relaxed));
        float thresh = 0.0f;
        switch (mode)
        {
            case 0:
            {
                const float peakRed = track.strip.compOptoPeakRed.load (std::memory_order_relaxed);
                thresh = -peakRed * (60.0f / 100.0f);
                break;
            }
            case 1:
            {
                const float drive = track.strip.compFetInput.load (std::memory_order_relaxed);
                thresh = -drive;
                break;
            }
            case 2:
            default:
                thresh = track.strip.compVcaThreshDb.load (std::memory_order_relaxed);
                break;
        }
        const float y = yForDb (thresh, inputBarArea);

        // Bigger, brighter triangle - the previous 8-px-tall handle was hard
        // to spot against a busy meter. We render the triangle so the tip
        // SLIGHTLY overlaps the IN bar (extends past handleArea) for an
        // unmistakable visual hand-off between the marker and the level.
        const float halfH = 7.0f;          // up from 4
        const float baseX = handleArea.getX();
        const float tipX  = handleArea.getRight() + 2.0f;  // overshoot into bar
        juce::Path tri;
        tri.addTriangle (baseX, y - halfH,
                         baseX, y + halfH,
                         tipX,  y);

        const bool engaged = track.strip.compEnabled.load (std::memory_order_relaxed);
        const auto fill   = engaged ? juce::Colour (fourKColors::kCompGold).brighter (0.30f)
                                     : juce::Colour (0xff909098);
        const auto outline = juce::Colour (0xff0a0a0a);

        // Subtle drop-shadow / glow so the triangle pops against the dark
        // strip background even when the comp is bypassed.
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        juce::Path shadow;
        shadow.addTriangle (baseX, y - halfH + 1.0f,
                             baseX, y + halfH + 1.0f,
                             tipX + 1.0f, y + 1.0f);
        g.fillPath (shadow);

        g.setColour (fill);
        g.fillPath (tri);
        g.setColour (outline);
        g.strokePath (tri, juce::PathStrokeType (1.0f));

        // Brighter horizontal threshold line across the IN bar.
        g.setColour (juce::Colour (0xfff8c878).withAlpha (engaged ? 0.75f : 0.40f));
        g.drawLine (inputBarArea.getX() - 1.0f, y,
                     inputBarArea.getRight() + 1.0f, y, 1.1f);
    }
}

// Translate a "threshold dB" drag value into the per-mode parameter the
// engine actually reads. Mirrors the mapping ChannelCompEditor uses, so the
// THR triangle on the strip and the THRESHOLD rotary in the comp editor
// dialog produce the same audible effect.
//
//   VCA  -> compVcaThreshDb directly (clamped to its -38..12 range)
//   Opto -> compOptoPeakRed  (knob 0 dB = 0 % reduction, -60 dB = 100 %)
//   FET  -> chain compFetInput up and compFetOutput down by the same dB
//           so the drive into the FET stage rises while net level holds
static void writeThresholdForMode (Track& t, float threshDb)
{
    const int mode = juce::jlimit (0, 2, t.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0:
        {
            const float peakRed = juce::jlimit (0.0f, 100.0f, -threshDb * (100.0f / 60.0f));
            t.strip.compOptoPeakRed.store (peakRed, std::memory_order_relaxed);
            break;
        }
        case 1:
        {
            // FET donor uses a FIXED -10 dBFS threshold; only the input
            // gain knob exists. Make the meter-strip threshold drag feel
            // like Opto / VCA (drop marker -> more compression, net level
            // roughly unchanged) by pairing the input drive with an
            // equal-and-opposite output trim. compFetOutput clamps at
            // -20 dB so heavy drives past +20 dB drift hot, which is fine
            // -- that corner is "deliberately driving the 1176". The
            // dedicated IN knob in the comp editor still moves the input
            // independently for 1176-style drive workflows.
            const float drive = juce::jlimit (0.0f, 40.0f, -threshDb);
            const float comp  = juce::jlimit (-20.0f, 20.0f, -drive);
            t.strip.compFetInput .store (drive, std::memory_order_relaxed);
            t.strip.compFetOutput.store (comp,  std::memory_order_relaxed);
            break;
        }
        case 2:
        default:
            t.strip.compVcaThreshDb.store (juce::jlimit (-38.0f, 12.0f, threshDb),
                                            std::memory_order_relaxed);
            break;
    }
}

void CompMeterStrip::mouseDown (const juce::MouseEvent& e)
{
    // Drag anywhere on the LEFT half (handle column + IN bar) sets threshold,
    // so the engineer can slap the bar at the desired level rather than
    // having to hit the small triangle.
    draggingThreshold = (e.x <= (int) inputBarArea.getRight() + 2);
    if (draggingThreshold)
    {
        const float db = dbForY (e.y, inputBarArea);
        writeThresholdForMode (track, db);
        // Auto-enable the comp when the engineer touches threshold - matches
        // the popup-editor behaviour and saves the "I moved threshold but
        // nothing happened" round-trip to flick the ON button. The strip's
        // timerCallback re-syncs the visual ON state on its next tick.
        track.strip.compEnabled.store (true, std::memory_order_relaxed);
        repaint();
    }
}

void CompMeterStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingThreshold) return;
    const float db = dbForY (e.y, inputBarArea);
    writeThresholdForMode (track, db);
    track.strip.compEnabled.store (true, std::memory_order_relaxed);
    repaint();
}

void CompMeterStrip::mouseUp (const juce::MouseEvent&)
{
    draggingThreshold = false;
}

// True "no compression" reset for the active mode. Distinct from
// writeThresholdForMode(track, 0.0f) because the drag mapping treats 0 dB
// threshold differently per mode:
//   Opto  - 0 dB drag -> 0 % peak reduction -> genuinely no compression
//   FET   - 0 dB drag -> 0 dB drive into FET -> genuinely no compression
//   VCA   - 0 dB drag -> 0 dB threshold, which still compresses every
//           signal above 0 dBFS. To get neutral on VCA we set threshold
//           to its +12 dB ceiling (the range is -38..12 in Session.h).
static void resetThresholdForMode (Track& t)
{
    const int mode = juce::jlimit (0, 2, t.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0:
            t.strip.compOptoPeakRed.store (0.0f, std::memory_order_relaxed);
            break;
        case 1:
            t.strip.compFetInput .store (0.0f, std::memory_order_relaxed);
            t.strip.compFetOutput.store (0.0f, std::memory_order_relaxed);
            break;
        case 2:
        default:
            t.strip.compVcaThreshDb.store (12.0f, std::memory_order_relaxed);
            break;
    }
}

void CompMeterStrip::mouseDoubleClick (const juce::MouseEvent&)
{
    resetThresholdForMode (track);
    repaint();
}
} // namespace focal

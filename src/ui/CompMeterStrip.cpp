#include "CompMeterStrip.h"
#include "DuskStudioLookAndFeel.h"

namespace duskstudio
{
CompMeterStrip::CompMeterStrip (Source s) : src (std::move (s))
{
    setOpaque (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    startTimerHz (30);
}

// Translate a "threshold dB" drag value into the per-mode parameter the
// engine actually reads. Mirrors the mapping ChannelCompEditor uses, so the
// THR triangle on the strip and the THRESHOLD rotary in the comp editor
// dialog produce the same audible effect.
//
//   VCA  -> compVcaThreshDb directly (clamped to its -38..12 range)
//   Opto -> compOptoPeakRed  (knob 0 dB = 0 % reduction, -60 dB = 100 %)
//   FET  -> compFetThresholdDb (donor's adjustable fet_threshold). Output
//           knob is independent, drive is independent — touching threshold
//           sets the real detection threshold, not a drive amount.
//           Matches ChannelCompEditor::writeThresholdToMode.
void CompMeterStrip::writeThresholdForMode (Track& t, float threshDb)
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
            t.strip.compFetThresholdDb.store (juce::jlimit (-60.0f, 0.0f, threshDb),
                                                std::memory_order_relaxed);
            break;
        }
        case 2:
        default:
            t.strip.compVcaThreshDb.store (juce::jlimit (-38.0f, 12.0f, threshDb),
                                            std::memory_order_relaxed);
            break;
    }
}

float CompMeterStrip::readThresholdForMode (const Track& t)
{
    const int mode = juce::jlimit (0, 2, t.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0:
        {
            const float peakRed = t.strip.compOptoPeakRed.load (std::memory_order_relaxed);
            return -peakRed * (60.0f / 100.0f);
        }
        case 1:
            return t.strip.compFetThresholdDb.load (std::memory_order_relaxed);
        case 2:
        default:
            return t.strip.compVcaThreshDb.load (std::memory_order_relaxed);
    }
}

// True "no compression" reset for the active mode. Distinct from
// writeThresholdForMode(track, 0.0f) because the drag mapping treats 0 dB
// threshold differently per mode:
//   Opto  - 0 dB drag -> 0 % peak reduction -> genuinely no compression
//   FET   - 0 dB drag -> 0 dB drive into FET -> genuinely no compression
//   VCA   - 0 dB drag -> 0 dB threshold, which still compresses every
//           signal above 0 dBFS. To get neutral on VCA we set threshold
//           to its +12 dB ceiling (the range is -38..12 in Session.h).
void CompMeterStrip::resetThresholdForMode (Track& t)
{
    const int mode = juce::jlimit (0, 2, t.strip.compMode.load (std::memory_order_relaxed));
    switch (mode)
    {
        case 0:
            t.strip.compOptoPeakRed.store (0.0f, std::memory_order_relaxed);
            break;
        case 1:
            // FET: reset the detector side only — threshold + input drive to
            // 0 dB (no compression). Leave compFetOutput (makeup) alone: this
            // is a double-click on the THRESHOLD handle, so dumping the user's
            // makeup level would be a surprising jump. Matches Opto/VCA, which
            // also reset only their threshold-equivalent.
            t.strip.compFetThresholdDb.store (0.0f, std::memory_order_relaxed);
            t.strip.compFetInput      .store (0.0f, std::memory_order_relaxed);
            break;
        case 2:
        default:
            t.strip.compVcaThreshDb.store (12.0f, std::memory_order_relaxed);
            break;
    }
}

// Track-bound convenience: builds the per-mode Source from a Track& so
// the channel-strip call sites don't change.
CompMeterStrip::CompMeterStrip (Track& t)
    : CompMeterStrip (Source {
          /*getInputDb*/   [&t] { return t.meterInputDb.load (std::memory_order_relaxed); },
          /*getGrDb*/      [&t] { return t.meterGrDb   .load (std::memory_order_relaxed); },
          /*getThresholdDb*/ [&t] { return readThresholdForMode (t); },
          /*setThresholdDb*/ [&t] (float db) { writeThresholdForMode (t, db); },
          /*resetThreshold*/ [&t] { resetThresholdForMode (t); },
          /*isEngaged*/      [&t] { return t.strip.compEnabled.load (std::memory_order_relaxed); },
          /*autoEnable*/     [&t] { t.strip.compEnabled.store (true, std::memory_order_relaxed); },
      })
{
}

CompMeterStrip::~CompMeterStrip() = default;

float CompMeterStrip::dbToFrac (float db) const noexcept
{
    return juce::jlimit (0.0f, 1.0f, (db - src.floorDb) / (src.ceilingDb - src.floorDb));
}

float CompMeterStrip::fracToDb (float frac) const noexcept
{
    return juce::jlimit (src.floorDb, src.ceilingDb,
                          src.floorDb + frac * (src.ceilingDb - src.floorDb));
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
    // Pure-GR mode (no IN bar, no handle) is used as a slim LED next to
    // a parent meter; the parent component owns its own "GR" caption, so
    // suppress the widget's internal one + its 10 px top reserve to keep
    // the GR bar flush with neighbouring meters.
    const bool pureGr = (! showInputBar) && (! showHandle);
    hasCaptions = (! pureGr) && (getLocalBounds().getHeight() >= 60);
    if (hasCaptions)
        b.removeFromTop (10.0f);

    const float W = b.getWidth();
    const float gap = juce::jmax (1.0f, W * 0.04f);

    if (! showInputBar)
    {
        // Slim layout — used by the fader-side track variant. With the
        // handle hidden the whole widget collapses to a pure GR bar so
        // it can sit next to the main level meter as a skinny GR LED
        // (master-strip-style).
        if (showHandle)
        {
            const float handleW = juce::jlimit (10.0f, 18.0f, W * 0.38f);
            if (handleOnRight)
            {
                handleArea = b.removeFromRight (handleW);
                b.removeFromRight (gap);
            }
            else
            {
                handleArea = b.removeFromLeft (handleW);
                b.removeFromLeft (gap);
            }
        }
        else
        {
            handleArea = {};
        }
        inputBarArea = {};
        scaleArea    = {};
        grBarArea    = b;
        return;
    }

    const float handleW = juce::jlimit (5.0f, 12.0f, W * 0.18f);
    const float scaleW  = (W >= 32.0f) ? juce::jlimit (10.0f, 16.0f, W * 0.22f) : 0.0f;
    const float barsW   = W - handleW - scaleW - gap * (scaleW > 0 ? 3.0f : 2.0f);
    const float barW    = juce::jmax (4.0f, barsW / 2.0f);
    handleArea   = b.removeFromLeft (handleW);
    b.removeFromLeft (gap);
    inputBarArea = b.removeFromLeft (barW);
    if (scaleW > 0.0f)
    {
        b.removeFromLeft (gap);
        scaleArea = b.removeFromLeft (scaleW);
    }
    else
    {
        scaleArea = {};
    }
    b.removeFromLeft (gap);
    grBarArea    = b.removeFromLeft (barW);
}

void CompMeterStrip::setShowInputBar (bool s)
{
    if (showInputBar == s) return;
    showInputBar = s;
    resized();
    repaint();
}

void CompMeterStrip::setHandleVisible (bool s)
{
    if (showHandle == s) return;
    showHandle = s;
    resized();
    repaint();
}

void CompMeterStrip::setHandleOnRight (bool right)
{
    if (handleOnRight == right) return;
    handleOnRight = right;
    resized();
    repaint();
}

void CompMeterStrip::timerCallback()
{
    const float inputDb = src.getInputDb ? src.getInputDb() : -100.0f;
    if (inputDb > displayedInputDb)
        displayedInputDb = inputDb;
    else
        displayedInputDb += (inputDb - displayedInputDb) * 0.15f;

    if (inputDb >= inputPeakHoldDb)
    {
        inputPeakHoldDb = inputDb;
        inputPeakHoldFrames = 18;
    }
    else if (inputPeakHoldFrames > 0)
    {
        --inputPeakHoldFrames;
    }
    else
    {
        inputPeakHoldDb = juce::jmax (src.floorDb, inputPeakHoldDb - 1.5f);
    }

    // Force the GR meter to read 0 whenever the compressor is bypassed.
    // The DSP holds the last-computed gainReduction value when bypassed,
    // so without this gate the LED would freeze at whatever reduction
    // the comp was applying at the moment it was turned off.
    const bool engaged = src.isEngaged ? src.isEngaged() : true;
    // Treat the input bar as silent below -50 dB. The donor's detector
    // also holds its last-computed gain reduction even when the input
    // signal stops (no fresh samples to release against), so without
    // this silence gate the LED would freeze at whatever reduction the
    // comp was applying at the moment playback stopped.
    constexpr float kSilenceFloor = -50.0f;
    const bool inputSilent = displayedInputDb < kSilenceFloor;
    const float gr = (engaged && ! inputSilent && src.getGrDb) ? src.getGrDb() : 0.0f;
    if (! engaged || inputSilent)
        displayedGrDb = 0.0f;
    else if (gr < displayedGrDb)
        displayedGrDb = gr;
    else
        displayedGrDb += (gr - displayedGrDb) * 0.18f;
    // Snap the decaying tail to exactly 0 once we're within a fraction of
    // a dB — prevents a tiny residual fill that looks like a stuck LED.
    if (displayedGrDb > -0.05f) displayedGrDb = 0.0f;

    repaint();
}

namespace
{
// Renders one segmented-LED bar - dark background, gradient fill, and thin
// black gridlines that give the stacked-LED look from hardware level meters.
void drawSegmentedBar (juce::Graphics& g,
                        juce::Rectangle<float> bar, float frac,
                        juce::Colour topColour, juce::Colour bottomColour,
                        bool fillFromTop,
                        int forcedSegments = -1)
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

    const int segments = (forcedSegments > 0)
                            ? forcedSegments
                            : juce::jlimit (8, 30, (int) (bar.getHeight() / 3.5f));
    const float segStep = bar.getHeight() / (float) segments;
    g.setColour (juce::Colour (0xff020203));
    for (int i = 1; i < segments; ++i)
    {
        const float yy = bar.getY() + i * segStep;
        g.fillRect (juce::Rectangle<float> (bar.getX() + 1.0f, yy - 0.4f,
                                              bar.getWidth() - 2.0f, 0.8f));
    }

    g.setColour (juce::Colour (0xff2a2a30));
    g.drawRoundedRectangle (bar, 1.5f, 0.5f);
}
} // namespace

void CompMeterStrip::paint (juce::Graphics& g)
{
    if (showInputBar)
    {
        // Full-widget background only when the IN bar is visible (the
        // background visually frames the bar pair). In slim GR-only mode
        // the handle column would otherwise read as a phantom IN bar to
        // the left of the GR LED — skip the full fill and let the
        // background show through, painting bg only behind the GR bar.
        auto bg = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff141418));
        g.fillRoundedRectangle (bg, 2.0f);
        g.setColour (juce::Colour (0xff2a2a30));
        g.drawRoundedRectangle (bg, 2.0f, 0.6f);
    }
    else if (! grBarArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff141418));
        g.fillRoundedRectangle (grBarArea, 2.0f);
        g.setColour (juce::Colour (0xff2a2a30));
        g.drawRoundedRectangle (grBarArea, 2.0f, 0.6f);
    }

    if (hasCaptions && grBarArea.getWidth() > 4.0f)
    {
        static const juce::Font kCaptionFont (juce::FontOptions (8.0f, juce::Font::bold));
        g.setFont (kCaptionFont);
        g.setColour (juce::Colour (0xff909098));
        if (showInputBar && inputBarArea.getWidth() > 4.0f)
            g.drawText ("IN",
                         juce::Rectangle<float> (inputBarArea.getX() - 1.0f, 0.0f,
                                                   inputBarArea.getWidth() + 2.0f, 9.0f),
                         juce::Justification::centred, false);
        // "GR" needs ~13 px at this font size — when the bar is slim
        // (e.g. the 9-px master-style LED) the natural bar width can't
        // hold both letters, so widen the text rect (allowed to overflow
        // into adjacent transparent space) instead of truncating to "G".
        const float grCaptionW = juce::jmax (grBarArea.getWidth() + 2.0f, 16.0f);
        g.drawText ("GR",
                     juce::Rectangle<float> (grBarArea.getCentreX() - grCaptionW * 0.5f,
                                               0.0f,
                                               grCaptionW, 9.0f),
                     juce::Justification::centred, false);
    }

    if (! scaleArea.isEmpty())
    {
        struct Tick { float db; const char* lbl; bool zero; };
        static constexpr Tick kTicks[] = {
            { 0.0f,   "0",  true  },
            { -3.0f,  "3",  false },
            { -6.0f,  "6",  false },
            { -12.0f, "12", false },
            { -20.0f, "20", false },
            { -40.0f, "40", false },
        };
        static const juce::Font kScaleFont (juce::FontOptions (7.0f));
        g.setFont (kScaleFont);
        for (auto& t : kTicks)
        {
            if (t.db < src.floorDb || t.db > src.ceilingDb) continue;
            const float y = yForDb (t.db, inputBarArea);
            g.setColour (t.zero ? juce::Colour (0xfff0e0a0)
                                 : juce::Colour (0xff6a6a72));
            g.drawLine (scaleArea.getX(), y, scaleArea.getRight() - 1.0f, y, 0.6f);
            g.setColour (t.zero ? juce::Colour (0xffffffff)
                                 : juce::Colour (0xffb0b0b8));
            g.drawText (t.lbl,
                        juce::Rectangle<float> (scaleArea.getX(), y - 5.0f,
                                                  scaleArea.getWidth() - 1.0f, 10.0f),
                        juce::Justification::centred, false);
        }
    }

    if (showInputBar && ! inputBarArea.isEmpty())
    {
        const float frac = dbToFrac (displayedInputDb);
        drawSegmentedBar (g, inputBarArea, frac,
                          juce::Colour (0xffe05050),
                          juce::Colour (0xff44d058),
                          /*fillFromTop=*/false);

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

    {
        const float grAbs = juce::jlimit (0.0f, std::abs (kGrFloorDb),
                                            std::abs (displayedGrDb));
        const float frac = grAbs / std::abs (kGrFloorDb);
        // Force one segment per dB of reduction (24 segments for the
        // 0..-24 dB range) so each visible LED step represents 1 dB —
        // lets the user read GR amount off the bar directly without a
        // dB tick column.
        const int grSegments = (int) std::abs (kGrFloorDb);
        drawSegmentedBar (g, grBarArea, frac,
                          juce::Colour (fourKColors::kCompGold).brighter (0.25f),
                          juce::Colour (fourKColors::kHfRed).brighter (0.10f),
                          /*fillFromTop=*/true,
                          grSegments);
    }

    if (showHandle)
    {
        // Threshold Y reference: input bar when present, otherwise the GR
        // bar (same vertical extent, used purely as a Y mapping target).
        const auto refBar = (showInputBar && ! inputBarArea.isEmpty()) ? inputBarArea
                                                                        : grBarArea;
        const float thresh = src.getThresholdDb ? src.getThresholdDb() : 0.0f;
        const float y = yForDb (thresh, refBar);

        // Bigger, more prominent triangle when the IN bar is hidden — the
        // fader-side layout needs the handle to read clearly without a
        // neighbouring bar to anchor against. Tip points TOWARD the bar
        // — that's RIGHT when the handle sits on the left of the bar
        // (default) and LEFT when handleOnRight=true (fader-side layout).
        const float halfH = showInputBar ? 7.0f : 11.0f;
        const float baseX = handleOnRight ? handleArea.getRight()
                                          : handleArea.getX();
        const float tipX  = handleOnRight ? handleArea.getX()    - 2.0f
                                          : handleArea.getRight() + 2.0f;
        juce::Path tri;
        tri.addTriangle (baseX, y - halfH,
                         baseX, y + halfH,
                         tipX,  y);

        const bool engaged = src.isEngaged ? src.isEngaged() : false;
        const auto fill   = engaged ? juce::Colour (fourKColors::kCompGold).brighter (0.30f)
                                     : juce::Colour (0xff909098);
        const auto outline = juce::Colour (0xff0a0a0a);

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        juce::Path shadow;
        shadow.addTriangle (baseX, y - halfH + 1.0f,
                             baseX, y + halfH + 1.0f,
                             tipX + 1.0f, y + 1.0f);
        g.fillPath (shadow);

        g.setColour (fill);
        g.fillPath (tri);
        g.setColour (outline);
        g.strokePath (tri, juce::PathStrokeType (showInputBar ? 1.0f : 1.4f));

        g.setColour (juce::Colour (0xfff8c878).withAlpha (engaged ? 0.75f : 0.40f));
        g.drawLine (refBar.getX() - 1.0f, y,
                     refBar.getRight() + 1.0f, y, 1.1f);
    }
}

void CompMeterStrip::mouseDown (const juce::MouseEvent& e)
{
    if (! showHandle) return;   // pure-GR mode — no threshold drag
    // Hit area = union of bar + handle column so clicks land whichever
    // side the triangle is on. Without this, flipping handleOnRight=true
    // (channel-strip fader-side layout) broke drag because the old test
    // only accepted clicks inside the bar's left half.
    const auto refBar = (showInputBar && ! inputBarArea.isEmpty()) ? inputBarArea
                                                                    : grBarArea;
    const auto hit = refBar.getUnion (handleArea).expanded (2.0f, 0.0f);
    draggingThreshold = hit.contains ((float) e.x, (float) e.y);
    if (draggingThreshold)
    {
        const float db = dbForY (e.y, refBar);
        if (src.setThresholdDb) src.setThresholdDb (db);
        if (src.autoEnable)     src.autoEnable();
        repaint();
    }
}

void CompMeterStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingThreshold) return;
    const auto refBar = (showInputBar && ! inputBarArea.isEmpty()) ? inputBarArea
                                                                    : grBarArea;
    const float db = dbForY (e.y, refBar);
    if (src.setThresholdDb) src.setThresholdDb (db);
    if (src.autoEnable)     src.autoEnable();
    repaint();
}

void CompMeterStrip::mouseUp (const juce::MouseEvent&)
{
    draggingThreshold = false;
}

void CompMeterStrip::mouseDoubleClick (const juce::MouseEvent&)
{
    if (src.resetThreshold) src.resetThreshold();
    repaint();
}
} // namespace duskstudio

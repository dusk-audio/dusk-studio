#pragma once

#include "RepaintGate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// The console's LED level bars: the ballistics their timers run and what paint()
// draws from the result, in one place so each repaint gate keys on exactly what
// is drawn.
namespace duskstudio
{
// Fast attack, slow release, and a peak hold of 18 ticks (about 600 ms at 30 Hz)
// that then falls 1.5 dB a tick to the -100 dB floor.
struct LevelBallistics
{
    float displayed = -100.0f;
    float peak = -100.0f;
    int peakFrames = 0;

    void step (float incoming) noexcept
    {
        if (incoming > displayed) displayed = incoming;
        else                      displayed += (incoming - displayed) * 0.15f;

        if (incoming >= peak)
        {
            peak = incoming;
            peakFrames = 18;
        }
        else if (peakFrames > 0) --peakFrames;
        else peak = std::max (-100.0f, peak - 1.5f);
    }
};

// A fader's range with its skew about -12 dB, so a meter's LED ticks line up
// with the fader's labels. JUCE's setSkewFactorFromMidPoint formula:
// skew = log(0.5) / log(midFrac).
inline float faderSkewMeterFrac (float db, float minDb, float maxDb) noexcept
{
    static constexpr float kMidDb = -12.0f;
    const float midFrac = (kMidDb - minDb) / (maxDb - minDb);
    const float skewFactor = std::log (0.5f) / std::log (midFrac);
    const float t = std::clamp ((db - minDb) / (maxDb - minDb), 0.0f, 1.0f);
    return std::pow (t, skewFactor);
}

// The tip colour: green, yellow from -5 dB, red from +5 dB.
inline int levelZone (float db) noexcept { return db >= 5.0f ? 2 : db >= -5.0f ? 1 : 0; }

struct LevelBarDraw
{
    DrawnExtent fill, peak;
    int fillZone = 0;
    bool peakHot = false;   // the peak line turns pink at +5 dB
};

// A channel strip's input bar over `span` pixels: the fill and the peak line are
// each drawn once their fraction of the bar is over a thousandth.
inline LevelBarDraw inputBarDraw (float fillDb, float fillFrac,
                                  float peakDb, float peakFrac, float span) noexcept
{
    LevelBarDraw draw;
    draw.fill = drawnAbove (fillFrac, 0.001f, span * fillFrac);
    draw.peak = drawnAbove (peakFrac, 0.001f, span * peakFrac);
    draw.fillZone = levelZone (fillDb);
    draw.peakHot = peakDb >= 5.0f;
    return draw;
}

// A bus or master column over `span` pixels: no peak line, and the fill is drawn
// once it is over half a pixel tall.
inline LevelBarDraw outputColumnDraw (float db, float frac, float span) noexcept
{
    LevelBarDraw draw;
    const float px = span * frac;
    draw.fill = drawnAbove (px, 0.5f, px);
    draw.fillZone = levelZone (db);
    return draw;
}

struct LevelBarGate
{
    PaintedValue fill, peak, look;

    bool changed (const LevelBarDraw& draw) noexcept
    {
        const bool filled = fill.changed (draw.fill.key(), kSubPixelRepaint);
        const bool peaked = peak.changed (draw.peak.key(), kSubPixelRepaint);
        const bool looked = look.changed ((float) (draw.fillZone + 3 * (draw.peakHot ? 1 : 0)), 0.0f);
        return filled || peaked || looked;
    }

    void forget() noexcept
    {
        fill.forget();
        peak.forget();
        look.forget();
    }
};

// The one-decimal peak readout under a meter, "-inf" at -60 dB and below. The
// label shows this text, so the text is its own repaint key.
struct PeakReadout
{
    char text[16] = {};

    bool update (float db) noexcept
    {
        char next[sizeof text];
        if (db <= -60.0f) std::snprintf (next, sizeof next, "-inf");
        else              std::snprintf (next, sizeof next, "%.1f", static_cast<double> (db));
        if (std::strcmp (next, text) == 0) return false;
        std::memcpy (text, next, sizeof text);
        return true;
    }
};
} // namespace duskstudio

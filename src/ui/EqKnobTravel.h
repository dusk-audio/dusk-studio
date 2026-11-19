#pragma once

#if DUSKSTUDIO_HAS_DUSK_DSP
 #include <dsp/FourKEQDSP.hpp>
#endif

#include <algorithm>
#include <cmath>

namespace duskstudio
{
enum class EqKnob { LfFreq, LmFreq, HmFreq, HfFreq, Q };

// How far a channel EQ band's frequency knob, or a Q knob, has turned for a
// stored value (0 to 1), and back. With the donor core the knobs travel as
// 4K EQ 2's do: through the 21 positions k/20 the console's captures were
// taken at, each carrying the core's anchor, and linearly between them. A
// frequency knob rises clockwise; a Q knob runs from its narrowest anchor at
// the left end to its broadest at the right. At any position the strip stores
// the number the plug-in's knob reads there. A stored value past the anchors,
// which MIDI, a control surface or an older session can leave, rests the knob
// on the nearer end until it moves.
//
// lo and hi are the range the knob's slider holds. Without the donor core the
// knob skews about mid between them, as JUCE's skew from a midpoint does.
struct EqKnobTravel
{
    EqKnob knob;
    double lo, hi, mid;

    double valueAt (double position) const noexcept
    {
        position = std::clamp (position, 0.0, 1.0);
#if DUSKSTUDIO_HAS_DUSK_DSP
        const float* a = anchors();
        const double turned = position * kLast;
        const double x = knob == EqKnob::Q ? kLast - turned : turned;
        const int i = std::min ((int) x, kLast - 1);
        return a[i] + (x - i) * ((double) a[i + 1] - a[i]);
#else
        return position > 0.0 ? lo + (hi - lo) * std::exp (std::log (position) / skew()) : lo;
#endif
    }

    double positionOf (double value) const noexcept
    {
#if DUSKSTUDIO_HAS_DUSK_DSP
        const float* a = anchors();
        double x = 0.0;
        if (! (value < a[kLast]))
            x = kLast;
        else if (value > a[0])
        {
            int i = 0;
            while (value > a[i + 1]) ++i;
            x = i + (value - a[i]) / ((double) a[i + 1] - a[i]);
        }
        return (knob == EqKnob::Q ? kLast - x : x) / kLast;
#else
        return std::pow (std::clamp ((value - lo) / (hi - lo), 0.0, 1.0), skew());
#endif
    }

private:
#if DUSKSTUDIO_HAS_DUSK_DSP
    using Core = duskaudio::FourKEQDSP;
    static constexpr int kLast = Core::kControlAnchorCount - 1;

    const float* anchors() const noexcept
    {
        switch (knob)
        {
            case EqKnob::LfFreq: return Core::frequencyControlAnchors (Core::Band::LF);
            case EqKnob::LmFreq: return Core::frequencyControlAnchors (Core::Band::LM);
            case EqKnob::HmFreq: return Core::frequencyControlAnchors (Core::Band::HM);
            case EqKnob::HfFreq: return Core::frequencyControlAnchors (Core::Band::HF);
            case EqKnob::Q:      break;
        }
        return Core::qControlAnchors();
    }
#else
    double skew() const noexcept { return std::log (0.5) / std::log ((mid - lo) / (hi - lo)); }
#endif
};

// Gives a slider this travel over [travel.lo, travel.hi], snapping to step.
template <typename Slider>
void followEqKnobTravel (Slider& knob, const EqKnobTravel& travel, double step)
{
    decltype (knob.getNormalisableRange()) range {
        travel.lo, travel.hi,
        [travel] (double, double, double position) { return travel.valueAt (position); },
        [travel] (double, double, double value) { return travel.positionOf (value); } };
    range.interval = step;
    knob.setNormalisableRange (range);
}
} // namespace duskstudio

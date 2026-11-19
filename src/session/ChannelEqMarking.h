#pragma once

#include "Session.h"

#if DUSKSTUDIO_HAS_DUSK_DSP
 #include <dsp/FourKEQDSP.hpp>
#endif

#include <algorithm>
#include <cmath>

namespace duskstudio
{
// The frequency marking at which channel EQ band (LF, LM, HM or HF) plays hz
// in voicing black: the one whose band sits where the core's Hz API puts a
// band asked for hz, a bell's centre or a shelf's corner. Held at the ends of
// the core's anchor table, past which the dial law plays flat. The strip runs
// LF and HF as shelves. Gain moves a band the same way through either API, so
// the reference gain stands for every gain. A build without the core has no
// dial law, and holds hz to the band's range.
inline float channelEqMarkingForHz (ChannelStripParams::EqFreq band, float hz, bool black) noexcept
{
    using EqFreq = ChannelStripParams::EqFreq;
#if DUSKSTUDIO_HAS_DUSK_DSP
    using EQ = duskaudio::FourKEQDSP;
    const auto core = band == EqFreq::Lf ? EQ::Band::LF
                    : band == EqFreq::Lm ? EQ::Band::LM
                    : band == EqFreq::Hm ? EQ::Band::HM
                                         : EQ::Band::HF;
    const bool bell = core == EQ::Band::LM || core == EQ::Band::HM;
    const float designHz = EQ::calibratedEqFrequencyForHz (hz, EQ::kEqReferenceGainDb, core, black, bell);
    return EQ::controlForCalibratedEqFrequency (designHz, EQ::kEqReferenceGainDb, core, black, bell);
#else
    (void) black;
    const float lo = band == EqFreq::Lf ? ChannelStripParams::kLfFreqMin
                   : band == EqFreq::Lm ? ChannelStripParams::kLmFreqMin
                   : band == EqFreq::Hm ? ChannelStripParams::kHmFreqMin
                                        : ChannelStripParams::kHfFreqMin;
    const float hi = band == EqFreq::Lf ? ChannelStripParams::kLfFreqMax
                   : band == EqFreq::Lm ? ChannelStripParams::kLmFreqMax
                   : band == EqFreq::Hm ? ChannelStripParams::kHmFreqMax
                                        : ChannelStripParams::kHfFreqMax;
    return std::isfinite (hz) ? std::clamp (hz, lo, hi) : lo;
#endif
}
} // namespace duskstudio

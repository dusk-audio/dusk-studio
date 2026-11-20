#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/ChannelStrip.h"

#include <FourKEQDSP.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <vector>

using Catch::Matchers::WithinAbs;

// Response checks for the channel EQ as the app runs it: the 4K EQ core inside
// the strip's own oversampler at the Effect oversampling factor, HF a shelf. A
// band placed high rolls off early if its filters cramp close to Nyquist. The
// 4x render, where 20 kHz is a small fraction of the design rate, is the
// reference.

namespace
{
constexpr double kTwoPi = 6.28318530717958647692;
constexpr int    kBlock = 256;
constexpr float  kFrequencies[] = { 1000.0f, 6000.0f, 10000.0f, 13000.0f, 16000.0f, 18000.0f, 20000.0f };

enum class Band { None, Low, LowMid, HighMid, High };

struct Boost
{
    Band band = Band::None;
    float gainDb = 12.0f;
    float frequency = 0.0f;
    float hpf = 0.0f, lpf = 0.0f; // switched in when set
    bool black = false;
};

// A strip's settings for boost: by default the band under test is boosted
// 12 dB, HM at the top marking of its knob (7 kHz), HF at its top, 16 kHz.
// The frequencies are the knobs' markings, which the core plays through its
// dial law.
void setUp (duskstudio::ChannelStripParams& params, const Boost& boost)
{
    using duskstudio::ChannelStripParams;
    params.eqEnabled.store (true);
    const auto gain = [&boost] (Band band) { return boost.band == band ? boost.gainDb : 0.0f; };
    const auto freq = [&boost] (Band band, float fallback)
    {
        return boost.band == band && boost.frequency > 0.0f ? boost.frequency : fallback;
    };
    params.lfGainDb.store (gain (Band::Low));
    params.lfFreq.store (freq (Band::Low, 100.0f));
    params.lmGainDb.store (gain (Band::LowMid));
    params.lmFreq.store (freq (Band::LowMid, 600.0f));
    params.hmGainDb.store (gain (Band::HighMid));
    params.hmFreq.store (freq (Band::HighMid, ChannelStripParams::kHmFreqMax));
    params.hfGainDb.store (gain (Band::High));
    params.hfFreq.store (freq (Band::High, 16000.0f));
    params.hpfEnabled.store (boost.hpf > 0.0f);
    params.hpfFreq.store (boost.hpf > 0.0f ? boost.hpf : ChannelStripParams::kHpfOffHz);
    params.lpfEnabled.store (boost.lpf > 0.0f);
    params.lpfFreq.store (boost.lpf > 0.0f ? boost.lpf : ChannelStripParams::kLpfOffHz);
    params.eqBlackMode.store (boost.black);
}

int toneLength (double sampleRate)
{
    return (int) (sampleRate / 4.0) / kBlock * kBlock;
}

// A quiet sine, so the always-on console character stays linear.
std::vector<float> tone (double sampleRate, float frequency)
{
    std::vector<float> in ((size_t) toneLength (sampleRate));
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = 0.01f * (float) std::sin (kTwoPi * frequency * (double) i / sampleRate);
    return in;
}

// The steady-state gain from in to out, over the second half.
double gainDb (const std::vector<float>& in, const std::vector<float>& out, double sampleRate, float frequency)
{
    // Whole periods of the tone: a part period skews a low tone by 0.1 dB once
    // a filter shifts its phase.
    const int total = (int) in.size();
    const int periods = (int) ((total / 2) * (double) frequency / sampleRate);
    const int measured = (int) std::lround (periods * sampleRate / frequency);
    double inEnergy = 0.0, outEnergy = 0.0;
    for (int i = total - measured; i < total; ++i)
    {
        inEnergy  += (double) in[(size_t) i] * in[(size_t) i];
        outEnergy += (double) out[(size_t) i] * out[(size_t) i];
    }
    return 10.0 * std::log10 (outEnergy / inEnergy);
}

// Steady-state gain of a mono strip at its default fader and pan.
double stripGainDb (double sampleRate, int oversampling, Boost boost, float frequency)
{
    using duskstudio::ChannelStrip;
    duskstudio::ChannelStripParams params;
    setUp (params, boost);

    auto strip = std::make_unique<ChannelStrip>();
    strip->bind (params);
    strip->prepare (sampleRate, kBlock, oversampling);

    std::array<std::array<float, kBlock>, ChannelStrip::kNumBuses> busL {}, busR {};
    std::array<std::array<float, kBlock>, ChannelStrip::kNumAuxSends> auxL {}, auxR {};
    std::array<float*, ChannelStrip::kNumBuses> busLPtrs {}, busRPtrs {};
    std::array<float*, ChannelStrip::kNumAuxSends> auxLPtrs {}, auxRPtrs {};
    for (std::size_t i = 0; i < busLPtrs.size(); ++i) { busLPtrs[i] = busL[i].data(); busRPtrs[i] = busR[i].data(); }
    for (std::size_t i = 0; i < auxLPtrs.size(); ++i) { auxLPtrs[i] = auxL[i].data(); auxRPtrs[i] = auxR[i].data(); }

    const auto in = tone (sampleRate, frequency);
    std::vector<float> out (in.size());
    juce::MidiBuffer midi;
    std::array<float, kBlock> masterL {}, masterR {};
    for (int offset = 0; offset < (int) in.size(); offset += kBlock)
    {
        masterL.fill (0.0f);
        masterR.fill (0.0f);
        strip->processAndAccumulate (in.data() + offset, nullptr, midi, false,
                                     masterL.data(), masterR.data(),
                                     busLPtrs, busRPtrs, auxLPtrs, auxRPtrs, kBlock, true);
        std::copy (masterL.begin(), masterL.end(), out.begin() + offset);
    }
    return gainDb (in, out, sampleRate, frequency);
}

// Steady-state gain of a bare core set up as ChannelStrip::prepare sets up the
// strip's at 1x and handed the same settings, with each band's marking then
// given to the core's dial setter, as 4K EQ 2 hands over its parameter.
double coreGainDb (double sampleRate, Boost boost, float frequency)
{
    using EQ = duskaudio::FourKEQDSP;
    duskstudio::ChannelStripParams params;
    setUp (params, boost);

    EQ eq;
    eq.setOversampling (0);
    eq.setMsMode (false);
    eq.setAutoGain (false);
    eq.setBypass (false);
    eq.prepare (sampleRate, kBlock);
    eq.reset();
    duskstudio::ChannelStrip::pushEqParameters (eq, params);
    eq.setLfFreq (params.lfFreq.load());
    eq.setLmFreq (params.lmFreq.load());
    eq.setHmFreq (params.hmFreq.load());
    eq.setHfFreq (params.hfFreq.load());

    const auto in = tone (sampleRate, frequency);
    auto out = in;
    auto right = in;
    for (int offset = 0; offset < (int) in.size(); offset += kBlock)
    {
        float* lr[2] = { out.data() + offset, right.data() + offset };
        eq.processBlock (lr, lr, 2, kBlock);
    }
    return gainDb (in, out, sampleRate, frequency);
}

// What the band adds over the same strip with the EQ flat at the same factor. The
// fader, pan law, console character and the oversampler's own top-of-band droop
// cancel out.
double boostDb (double sampleRate, int oversampling, Boost boost, float frequency)
{
    return stripGainDb (sampleRate, oversampling, boost, frequency)
         - stripGainDb (sampleRate, oversampling, {}, frequency);
}

double boostDb (double sampleRate, int oversampling, Band band, float frequency)
{
    return boostDb (sampleRate, oversampling, Boost { band }, frequency);
}
} // namespace

TEST_CASE ("Channel EQ at 2x oversampling holds the HM bell's 4x curve up to 20 kHz", "[dsp][eq]")
{
    for (const double sampleRate : { 44100.0, 48000.0 })
    {
        CAPTURE (sampleRate);
        for (const float frequency : kFrequencies)
        {
            CAPTURE (frequency);
            CHECK_THAT (boostDb (sampleRate, 2, Band::HighMid, frequency),
                        WithinAbs (boostDb (sampleRate, 4, Band::HighMid, frequency), 1.0));
        }
        // A strip that ran every factor at 1x would pass the comparison above.
        CHECK (boostDb (sampleRate, 4, Band::HighMid, 20000.0f) > 7.0);
    }
}

TEST_CASE ("Channel EQ at the default 1x holds the HF shelf's 4x curve up to 20 kHz", "[dsp][eq]")
{
    for (const double sampleRate : { 44100.0, 48000.0 })
    {
        CAPTURE (sampleRate);
        for (const float frequency : kFrequencies)
        {
            CAPTURE (frequency);
            CHECK_THAT (boostDb (sampleRate, 1, Band::High, frequency),
                        WithinAbs (boostDb (sampleRate, 4, Band::High, frequency), 1.0));
        }
        // A shelf that boosted nothing at either factor would pass the comparison above.
        CHECK (boostDb (sampleRate, 1, Band::High, 20000.0f) > 8.0);
    }
}

TEST_CASE ("Channel EQ at the default 1x holds the HM bell's 4x curve up to 20 kHz", "[dsp][eq]")
{
    for (const double sampleRate : { 44100.0, 48000.0 })
    {
        CAPTURE (sampleRate);
        for (const float frequency : kFrequencies)
        {
            CAPTURE (frequency);
            CHECK_THAT (boostDb (sampleRate, 1, Band::HighMid, frequency),
                        WithinAbs (boostDb (sampleRate, 4, Band::HighMid, frequency), 1.0));
        }
    }
}

// The strip hands each band's frequency marking to the core's dial setter, as
// 4K EQ 2 hands over its parameter, so a band plays where the console's dial
// law puts its marking, which differs from the marking and with the voicing:
// LM marked 1 kHz plays near 800 Hz in Brown and 590 Hz in Black. The strip's
// response matches a core driven through the dial setter at the frequency the
// law plays, at the marking itself and an octave either side, where playing
// the marking as Hz differs by decibels.
TEST_CASE ("Channel EQ plays each band's frequency marking through the core's dial law", "[dsp][eq]")
{
    using EQ = duskaudio::FourKEQDSP;
    struct Marked { Band band; EQ::Band core; bool bell; float marking; };
    const Marked bands[] {
        { Band::Low,     EQ::Band::LF, false,  110.0f },
        { Band::LowMid,  EQ::Band::LM, true,  1000.0f },
        { Band::HighMid, EQ::Band::HM, true,  4800.0f },
        { Band::High,    EQ::Band::HF, false, 8000.0f },
    };
    for (const bool black : { false, true })
        for (const auto& b : bands)
        {
            Boost boost { b.band, 12.0f, b.marking };
            boost.black = black;
            Boost flat;
            flat.black = black;
            const float played = EQ::calibratedEqFrequency (b.marking, boost.gainDb, b.core, black, b.bell);
            for (const float frequency : { played, b.marking, played / 2.0f, played * 2.0f })
            {
                CAPTURE (black, b.marking, played, frequency);
                const double strip = stripGainDb (48000.0, 1, boost, frequency) - stripGainDb (48000.0, 1, flat, frequency);
                const double core  = coreGainDb (48000.0, boost, frequency) - coreGainDb (48000.0, flat, frequency);
                CHECK_THAT (strip, WithinAbs (core, 0.01));
            }
        }
}

// A bell sits where the dial law puts its marking. At the reference gain the
// law was measured at, the core centres it exactly there, so a half-octave
// either side of it lifts the same.
TEST_CASE ("Channel EQ centres a bell where the dial law plays its marking", "[dsp][eq]")
{
    using duskstudio::ChannelStripParams;
    using EQ = duskaudio::FourKEQDSP;
    for (const Boost boost : { Boost { Band::LowMid, EQ::kEqReferenceGainDb, 600.0f },
                               Boost { Band::HighMid, EQ::kEqReferenceGainDb, ChannelStripParams::kHmFreqMax } })
    {
        const auto core = boost.band == Band::LowMid ? EQ::Band::LM : EQ::Band::HM;
        const float played = EQ::calibratedEqFrequency (boost.frequency, boost.gainDb, core, false, true);
        CAPTURE (boost.frequency, played);
        const double centre = boostDb (48000.0, 1, boost, played);
        const double below  = boostDb (48000.0, 1, boost, played / 1.41421356f);
        const double above  = boostDb (48000.0, 1, boost, played * 1.41421356f);
        CHECK (centre > 3.0);
        CHECK (centre > below);
        CHECK (centre > above);
        CHECK_THAT (below, WithinAbs (above, 0.3));
    }
}

// A shelf's corner is where the dial law puts its marking, the frequency the
// core's Hz API names for that marking. From 3 dB of boost or cut, roughly
// half to two-thirds of it is in there and nearly all of it two octaves
// further out, against the shelf's plateau more than a decade out.
TEST_CASE ("Channel EQ shelf has its corner where the dial law plays its marking", "[dsp][eq]")
{
    using EQ = duskaudio::FourKEQDSP;
    struct Shelf { Band band; EQ::Band core; float marking, plateau; };
    const Shelf shelves[] { { Band::High, EQ::Band::HF, 1500.0f, 20000.0f },
                            { Band::Low,  EQ::Band::LF,  400.0f,    25.0f } };
    for (const bool black : { false, true })
        for (const auto& shelf : shelves)
        {
            const float corner = EQ::hzForCalibratedEqControl (shelf.marking, shelf.core, black, false);
            const float twoOctaves = shelf.band == Band::High ? corner * 4.0f : corner / 4.0f;
            CAPTURE (black, shelf.marking, corner);
            REQUIRE (std::max (corner, shelf.plateau) / std::min (corner, shelf.plateau) > 10.0f);
            for (const float gainDb : { 3.0f, 7.5f, 15.0f, -3.0f, -7.5f, -15.0f })
            {
                CAPTURE (gainDb);
                Boost boost { shelf.band, gainDb, shelf.marking };
                boost.black = black;
                Boost flat;
                flat.black = black;
                const auto lift = [&] (float frequency)
                {
                    return stripGainDb (48000.0, 1, boost, frequency) - stripGainDb (48000.0, 1, flat, frequency);
                };
                const double plateau = lift (shelf.plateau);
                const double atCorner = lift (corner) / plateau;
                const double further = lift (twoOctaves) / plateau;
                CAPTURE (plateau, atCorner, further);
                CHECK (std::abs (plateau) > 0.5 * std::abs (gainDb));
                CHECK (atCorner > 0.4);
                CHECK (atCorner < 0.75);
                CHECK (further > 0.85);
                CHECK (further < 1.15);
            }
        }
}

// Gain moves a peaking band's centre the way the console's does, by up to about
// 2.5%, and not at all at the reference gain the dial law was measured at.
// Found on the core's own response at 4x, where the top of a wide bell is not
// tilted by the matched designs a 1x render uses near Nyquist.
TEST_CASE ("Channel EQ gain moves a bell's centre by at most about 2.5 percent", "[dsp][eq]")
{
    using EQ = duskaudio::FourKEQDSP;
    const std::array<float, 3> lowMidMarkings { 200.0f, 600.0f, 2000.0f };
    const std::array<float, 3> highMidMarkings { 600.0f, 3000.0f, 7000.0f };
    double widest = 0.0;
    for (const bool black : { false, true })
        for (const bool high : { false, true })
            for (const float marking : high ? highMidMarkings : lowMidMarkings)
                for (const float gainDb : { 1.5f, 3.0f, 7.5f, 12.0f, 15.0f, -3.0f, -7.5f, -15.0f })
                {
                    EQ::CurveControls c;
                    c.baseSampleRate = 48000.0;
                    c.oversampling = 2.0f;
                    c.black = black;
                    c.lfFreq = 100.0f; c.lmFreq = 600.0f; c.hmFreq = 3000.0f; c.hfFreq = 8000.0f;
                    c.lmQ = c.hmQ = 0.7f;
                    (high ? c.hmFreq : c.lmFreq) = marking;
                    (high ? c.hmGain : c.lmGain) = gainDb;
                    const auto curve = EQ::designCurve (c);
                    const float atReference = EQ::calibratedEqFrequency (marking, EQ::kEqReferenceGainDb,
                                                                         high ? EQ::Band::HM : EQ::Band::LM, black, true);
                    float centre = atReference, extreme = 0.0f;
                    for (int i = -400; i <= 400; ++i)
                    {
                        const float f = atReference * std::pow (1.0005f, (float) i);
                        const float lift = EQ::curveDbAt (curve, f) * (gainDb > 0.0f ? 1.0f : -1.0f);
                        if (lift > extreme) { extreme = lift; centre = f; }
                    }
                    const double moved = centre / atReference - 1.0;
                    CAPTURE (black, high, marking, atReference, gainDb, moved);
                    CHECK (std::abs (moved) < 0.03);
                    if (std::abs (gainDb - EQ::kEqReferenceGainDb) < 0.01f)
                        CHECK (std::abs (moved) < 0.003);
                    widest = std::max (widest, std::abs (moved));
                }
    CHECK (widest > 0.015);
}

// A filter is 3 dB down, against its own passband, at the frequency its knob
// shows. Driven as a dial position instead, an HPF at 80 Hz would be 3 dB down
// near 26 Hz and an LPF at 12 kHz near 19.6 kHz. Below the corner the E
// HPF falls at 12 dB per octave and the G HPF, a three-pole, at 18.
TEST_CASE ("Channel filters are 3 dB down at the frequency their knob shows", "[dsp][eq]")
{
    constexpr double kHalfPowerDb = -3.0103;
    for (const bool black : { false, true })
    {
        CAPTURE (black);
        for (const float hpf : { 80.0f, 300.0f })
        {
            CAPTURE (hpf);
            Boost filter;
            filter.hpf = hpf;
            filter.black = black;
            Boost flat;
            flat.black = black;
            const auto response = [&] (float frequency)
            {
                return stripGainDb (48000.0, 1, filter, frequency) - stripGainDb (48000.0, 1, flat, frequency);
            };
            CHECK_THAT (response (hpf) - response (4000.0f), WithinAbs (kHalfPowerDb, 0.05));
            CHECK_THAT (response (hpf / 2.0f) - response (hpf / 4.0f), WithinAbs (black ? 18.0 : 12.0, 0.6));
        }
        for (const float lpf : { 5000.0f, 12000.0f })
        {
            CAPTURE (lpf);
            for (const int oversampling : { 1, 4 })
            {
                CAPTURE (oversampling);
                Boost filter;
                filter.lpf = lpf;
                filter.black = black;
                Boost flat;
                flat.black = black;
                const auto response = [&] (float frequency)
                {
                    return stripGainDb (48000.0, oversampling, filter, frequency)
                         - stripGainDb (48000.0, oversampling, flat, frequency);
                };
                CHECK_THAT (response (lpf) - response (500.0f), WithinAbs (kHalfPowerDb, 0.05));
            }
        }
    }
}

// The oversampler's halfband filters start to close just below 20 kHz at a
// 44.1 kHz session rate, with the EQ flat; at 48 kHz 20 kHz is clear of them.
TEST_CASE ("Effect oversampling takes about 1 dB off 20 kHz at 44.1 kHz only", "[dsp][eq]")
{
    for (const int oversampling : { 2, 4 })
    {
        CAPTURE (oversampling);
        const auto droop = [oversampling] (double sampleRate, float frequency)
        {
            return stripGainDb (sampleRate, oversampling, {}, frequency)
                 - stripGainDb (sampleRate, 1, {}, frequency);
        };
        CHECK_THAT (droop (44100.0, 20000.0f), WithinAbs (-1.2, 0.5));
        CHECK_THAT (droop (44100.0, 18000.0f), WithinAbs (0.0, 0.3));
        CHECK_THAT (droop (48000.0, 20000.0f), WithinAbs (0.0, 0.3));
    }
}

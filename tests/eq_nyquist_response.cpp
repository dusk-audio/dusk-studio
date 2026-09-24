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

// Steady-state gain of a mono strip at its default fader and pan, fed a quiet
// sine so the always-on console character stays linear. By default the band
// under test is boosted 12 dB: HM at the top of its knob (7 kHz), HF at 16 kHz.
double stripGainDb (double sampleRate, int oversampling, Boost boost, float frequency)
{
    using duskstudio::ChannelStrip;
    using duskstudio::ChannelStripParams;
    ChannelStripParams params;
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

    auto strip = std::make_unique<ChannelStrip>();
    strip->bind (params);
    strip->prepare (sampleRate, kBlock, oversampling);

    std::array<std::array<float, kBlock>, ChannelStrip::kNumBuses> busL {}, busR {};
    std::array<std::array<float, kBlock>, ChannelStrip::kNumAuxSends> auxL {}, auxR {};
    std::array<float*, ChannelStrip::kNumBuses> busLPtrs {}, busRPtrs {};
    std::array<float*, ChannelStrip::kNumAuxSends> auxLPtrs {}, auxRPtrs {};
    for (std::size_t i = 0; i < busLPtrs.size(); ++i) { busLPtrs[i] = busL[i].data(); busRPtrs[i] = busR[i].data(); }
    for (std::size_t i = 0; i < auxLPtrs.size(); ++i) { auxLPtrs[i] = auxL[i].data(); auxRPtrs[i] = auxR[i].data(); }

    const int total = (int) (sampleRate / 4.0) / kBlock * kBlock;
    std::vector<float> in ((size_t) total), out ((size_t) total);
    for (int i = 0; i < total; ++i)
        in[(size_t) i] = 0.01f * (float) std::sin (kTwoPi * frequency * i / sampleRate);

    juce::MidiBuffer midi;
    std::array<float, kBlock> masterL {}, masterR {};
    for (int offset = 0; offset < total; offset += kBlock)
    {
        masterL.fill (0.0f);
        masterR.fill (0.0f);
        strip->processAndAccumulate (in.data() + offset, nullptr, midi, false,
                                     masterL.data(), masterR.data(),
                                     busLPtrs, busRPtrs, auxLPtrs, auxRPtrs, kBlock, true);
        std::copy (masterL.begin(), masterL.end(), out.begin() + offset);
    }

    // Whole periods of the tone: a part period skews a low tone by 0.1 dB once
    // a filter shifts its phase.
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

// A bell sits where its knob says. The two settings are ones a dial-position
// reading gets wrong: it would centre an LM knob at 600 Hz near 410 Hz, and HM
// would stop moving at 6.4 kHz. At the reference gain the core places the
// centre exactly, so a half-octave either side of it lifts the same.
TEST_CASE ("Channel EQ centres a bell on the frequency its knob shows", "[dsp][eq]")
{
    using duskstudio::ChannelStripParams;
    constexpr float kReferenceGainDb = 7.5f;
    for (const Boost boost : { Boost { Band::LowMid, kReferenceGainDb, 600.0f },
                               Boost { Band::HighMid, kReferenceGainDb, ChannelStripParams::kHmFreqMax } })
    {
        CAPTURE (boost.frequency);
        const double centre = boostDb (48000.0, 1, boost, boost.frequency);
        const double below  = boostDb (48000.0, 1, boost, boost.frequency / 1.41421356f);
        const double above  = boostDb (48000.0, 1, boost, boost.frequency * 1.41421356f);
        CHECK (centre > 3.0);
        CHECK (centre > below);
        CHECK (centre > above);
        CHECK_THAT (below, WithinAbs (above, 0.3));
    }
}

// A shelf's frequency is its corner. From 3 dB of boost or cut, roughly half to
// two-thirds of it is in there and nearly all of it two octaves further out,
// against the shelf's plateau more than a decade out. Driven as a dial
// position instead, a Brown HF knob at 8 kHz would put that corner near 2.7 kHz.
TEST_CASE ("Channel EQ shelf has its corner at the frequency its knob shows", "[dsp][eq]")
{
    struct Shelf { Band band; float knob, twoOctaves, plateau; };
    const Shelf shelves[] { { Band::High, 1500.0f, 6000.0f, 20000.0f },
                            { Band::Low,   400.0f,  100.0f,    25.0f } };
    for (const bool black : { false, true })
        for (const auto& shelf : shelves)
            for (const float gainDb : { 3.0f, 7.5f, 15.0f, -3.0f, -7.5f, -15.0f })
            {
                CAPTURE (black, shelf.knob, gainDb);
                Boost boost { shelf.band, gainDb, shelf.knob };
                boost.black = black;
                Boost flat;
                flat.black = black;
                const auto lift = [&] (float frequency)
                {
                    return stripGainDb (48000.0, 1, boost, frequency) - stripGainDb (48000.0, 1, flat, frequency);
                };
                const double plateau = lift (shelf.plateau);
                const double atKnob = lift (shelf.knob) / plateau;
                const double further = lift (shelf.twoOctaves) / plateau;
                CAPTURE (plateau, atKnob, further);
                CHECK (std::abs (plateau) > 0.5 * std::abs (gainDb));
                CHECK (atKnob > 0.4);
                CHECK (atKnob < 0.75);
                CHECK (further > 0.85);
                CHECK (further < 1.15);
            }
}

// Gain moves a peaking band's centre the way the console's does, by up to about
// 2.5%, and not at all at the reference gain the Hz API is exact at. Found on
// the core's own response at 4x, where the top of a wide bell is not tilted by
// the matched designs a 1x render uses near Nyquist.
TEST_CASE ("Channel EQ gain moves a bell's centre by at most about 2.5 percent", "[dsp][eq]")
{
    using EQ = duskaudio::FourKEQDSP;
    const std::array<float, 3> lowMidKnobs { 200.0f, 600.0f, 2000.0f };
    const std::array<float, 3> highMidKnobs { 600.0f, 3000.0f, 7000.0f };
    double widest = 0.0;
    for (const bool black : { false, true })
        for (const bool high : { false, true })
            for (const float knob : high ? highMidKnobs : lowMidKnobs)
                for (const float gainDb : { 1.5f, 3.0f, 7.5f, 12.0f, 15.0f, -3.0f, -7.5f, -15.0f })
                {
                    EQ::CurveControls c;
                    c.baseSampleRate = 48000.0;
                    c.oversampling = 2.0f;
                    c.black = black;
                    c.bandFrequenciesInHz = true;
                    c.lfFreq = 100.0f; c.lmFreq = 600.0f; c.hmFreq = 3000.0f; c.hfFreq = 8000.0f;
                    c.lmQ = c.hmQ = 0.7f;
                    (high ? c.hmFreq : c.lmFreq) = knob;
                    (high ? c.hmGain : c.lmGain) = gainDb;
                    const auto curve = EQ::designCurve (c);
                    float centre = knob, extreme = 0.0f;
                    for (int i = -400; i <= 400; ++i)
                    {
                        const float f = knob * std::pow (1.0005f, (float) i);
                        const float lift = EQ::curveDbAt (curve, f) * (gainDb > 0.0f ? 1.0f : -1.0f);
                        if (lift > extreme) { extreme = lift; centre = f; }
                    }
                    const double moved = centre / knob - 1.0;
                    CAPTURE (black, high, knob, gainDb, moved);
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

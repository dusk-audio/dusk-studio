#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/ChannelStrip.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

using Catch::Matchers::WithinAbs;

// Cramping checks for the channel EQ as the app runs it: the 4K EQ core inside
// the strip's own oversampler at the Effect oversampling factor, HF a shelf. A
// band placed high rolls off early when its filters are designed close to
// Nyquist. The 4x render, where 20 kHz is a small fraction of the design rate, is
// the reference.

namespace
{
constexpr double kTwoPi = 6.28318530717958647692;
constexpr int    kBlock = 256;
constexpr float  kFrequencies[] = { 1000.0f, 6000.0f, 10000.0f, 13000.0f, 16000.0f, 18000.0f, 20000.0f };

enum class Band { None, HighMid, High };

// Steady-state gain of a mono strip at its default fader and pan, fed a quiet
// sine so the always-on console character stays linear. The band under test is
// boosted 12 dB: HM at the top of its knob, HF at 16 kHz.
double stripGainDb (double sampleRate, int oversampling, Band band, float frequency)
{
    using duskstudio::ChannelStrip;
    duskstudio::ChannelStripParams params;
    params.eqEnabled.store (true);
    params.hmGainDb.store (band == Band::HighMid ? 12.0f : 0.0f);
    params.hmFreq.store (duskstudio::ChannelStripParams::kHmFreqMax);
    params.hfGainDb.store (band == Band::High ? 12.0f : 0.0f);
    params.hfFreq.store (16000.0f);

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

    double inEnergy = 0.0, outEnergy = 0.0;
    for (int i = total / 2; i < total; ++i)
    {
        inEnergy  += (double) in[(size_t) i] * in[(size_t) i];
        outEnergy += (double) out[(size_t) i] * out[(size_t) i];
    }
    return 10.0 * std::log10 (outEnergy / inEnergy);
}

// What the band adds over the same strip with the EQ flat at the same factor. The
// fader, pan law, console character and the oversampler's own top-of-band droop
// cancel out.
double boostDb (double sampleRate, int oversampling, Band band, float frequency)
{
    return stripGainDb (sampleRate, oversampling, band, frequency)
         - stripGainDb (sampleRate, oversampling, Band::None, frequency);
}
} // namespace

// 2x sits within a few tenths of a dB of 4x; the 1x shortfall this would catch
// is 5 dB and more at 20 kHz.
TEST_CASE ("Channel EQ at 2x oversampling holds the HM bell's 4x curve up to 20 kHz", "[dsp][eq]")
{
    for (const double sampleRate : { 44100.0, 48000.0 })
    {
        CAPTURE (sampleRate);
        for (const float frequency : kFrequencies)
        {
            CAPTURE (frequency);
            CHECK_THAT (boostDb (sampleRate, 2, Band::HighMid, frequency),
                        WithinAbs (boostDb (sampleRate, 4, Band::HighMid, frequency), 2.0));
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
                        WithinAbs (boostDb (sampleRate, 4, Band::High, frequency), 1.5));
        }
    }
}

// The limit MANUAL documents at the default 1x: at 20 kHz a +12 dB HM boost at
// the top of its range falls about 5 to 7 dB short of its 4x curve.
TEST_CASE ("Channel EQ at the default 1x cramps an HM bell set high near Nyquist", "[dsp][eq]")
{
    for (const double sampleRate : { 44100.0, 48000.0 })
    {
        CAPTURE (sampleRate);
        const double shortfall = boostDb (sampleRate, 4, Band::HighMid, 20000.0f)
                               - boostDb (sampleRate, 1, Band::HighMid, 20000.0f);
        CHECK_THAT (shortfall, WithinAbs (6.0, 2.5));
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
            return stripGainDb (sampleRate, oversampling, Band::None, frequency)
                 - stripGainDb (sampleRate, 1, Band::None, frequency);
        };
        CHECK_THAT (droop (44100.0, 20000.0f), WithinAbs (-1.2, 0.5));
        CHECK_THAT (droop (44100.0, 18000.0f), WithinAbs (0.0, 0.3));
        CHECK_THAT (droop (48000.0, 20000.0f), WithinAbs (0.0, 0.3));
    }
}

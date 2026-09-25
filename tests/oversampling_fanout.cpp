#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "dsp/BusStrip.h"
#include "dsp/ChannelStrip.h"
#include "dsp/MasterBus.h"
#include "session/Session.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

using duskstudio::ChannelStrip;

// MANUAL "Configuring audio > Advanced": Effect Oversampling raises the
// internal rate of the channel EQ and compressor, the bus compressor, and the
// master EQ and compressor. Each stage named there saturates, and a stage that
// saturates a 15 kHz tone at 48 kHz folds its 2nd and 3rd harmonics (30 and
// 45 kHz) back to 18 and 3 kHz. Run at 2x or 4x, those harmonics sit below the
// raised Nyquist and the downsampler removes them, so the folded tones fall
// away. That drop is what shows the factor reached the stage; the reported
// latency shows the unit's own oversampler took the factor.

namespace
{
constexpr double kSr     = 48000.0;
constexpr int    kBlock  = 512;
constexpr double kTwoPi  = 6.283185307179586;
constexpr double kProbeHz = 15000.0;
constexpr int    kSettleBlocks  = 24;
constexpr int    kMeasureBlocks = 16;

// Amplitude of the `hz` component, by a single-bin DFT over a Hann window.
double toneDb (const std::vector<float>& v, double hz)
{
    double re = 0.0, im = 0.0, wsum = 0.0;
    const auto n = v.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (kTwoPi * (double) i / (double) (n - 1));
        const double a = kTwoPi * hz * (double) i / kSr;
        re += w * v[i] * std::cos (a);
        im -= w * v[i] * std::sin (a);
        wsum += w;
    }
    return 20.0 * std::log10 (std::max (2.0 * std::hypot (re, im) / wsum, 1.0e-15));
}

// The louder of the two folded harmonics, relative to the probe tone.
double foldedDb (const std::vector<float>& out)
{
    const double h2 = toneDb (out, kSr - 2.0 * kProbeHz);
    const double h3 = toneDb (out, 3.0 * kProbeHz - kSr);
    return std::max (h2, h3) - toneDb (out, kProbeHz);
}

// Drives a stereo processor with the probe tone on both sides and returns the
// left output of the measured blocks.
std::vector<float> probe (const std::function<void (float*, float*, int)>& process, float amp)
{
    std::vector<float> l ((std::size_t) kBlock), r ((std::size_t) kBlock), out;
    double phase = 0.0;
    const double inc = kTwoPi * kProbeHz / kSr;
    for (int b = 0; b < kSettleBlocks + kMeasureBlocks; ++b)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            l[(std::size_t) i] = r[(std::size_t) i] = amp * (float) std::sin (phase);
            phase += inc;
        }
        process (l.data(), r.data(), kBlock);
        if (b >= kSettleBlocks) out.insert (out.end(), l.begin(), l.end());
    }
    return out;
}

double channelFolded (int factor, bool comp)
{
    duskstudio::ChannelStripParams params;
    if (comp)
    {
        params.compEnabled.store (true);
        params.compMode.store (1);
        params.compFetInput.store (40.0f);
    }
    ChannelStrip strip;
    strip.bind (params);
    strip.insertMode.store (ChannelStrip::kInsertEmpty);
    strip.prepare (kSr, kBlock, factor);

    std::array<float*, ChannelStrip::kNumBuses> busL {}, busR {};
    std::array<float*, ChannelStrip::kNumAuxSends> auxL {}, auxR {};
    std::vector<float> mL ((std::size_t) kBlock), mR ((std::size_t) kBlock);
    juce::MidiBuffer midi;
    return foldedDb (probe ([&] (float* l, float*, int n)
    {
        std::fill (mL.begin(), mL.end(), 0.0f);
        std::fill (mR.begin(), mR.end(), 0.0f);
        strip.processAndAccumulate (l, nullptr, midi, false, mL.data(), mR.data(),
                                    busL, busR, auxL, auxR, n, true);
        std::copy (mL.begin(), mL.end(), l);
    }, 0.5f));
}

double busFolded (int factor)
{
    duskstudio::BusParams params;
    params.compEnabled.store (true);
    params.compThreshDb.store (-20.0f);
    duskstudio::BusStrip strip;
    strip.prepare (kSr, kBlock, factor);
    strip.bind (params);
    return foldedDb (probe ([&] (float* l, float* r, int n) { strip.processInPlace (l, r, n); }, 0.5f));
}

double masterFolded (int factor, bool eq, bool comp)
{
    duskstudio::MasterBusParams params;
    params.eqEnabled.store (eq);
    params.compEnabled.store (comp);
    params.compThreshDb.store (-20.0f);
    duskstudio::MasterBus master;
    master.bind (params);
    master.prepare (kSr, kBlock, factor);
    return foldedDb (probe ([&] (float* l, float* r, int n) { master.processInPlace (l, r, n); }, 0.5f));
}

// The half-band round trip: 23 samples at 2x, 26.5 rounded at 4x.
int expectedLatency (int factor)
{
    return factor == 4 ? 27 : (factor == 2 ? 23 : 0);
}
} // namespace

TEST_CASE ("Effect oversampling raises the channel EQ and compressor rate", "[oversampling]")
{
    const double eqNative   = channelFolded (1, false);
    const double compNative = channelFolded (1, true);
    CAPTURE (eqNative, compNative);
    // The EQ core's console saturator always runs, so it is the EQ stage the
    // comp-off probe hears. The FET compressor driven hard folds far more than
    // the saturator does, so the comp-on probe measures the compressor.
    REQUIRE (eqNative > -90.0);
    REQUIRE (compNative - eqNative > 20.0);

    for (const int factor : { 2, 4 })
    {
        ChannelStrip strip;
        duskstudio::ChannelStripParams params;
        strip.bind (params);
        strip.prepare (kSr, kBlock, factor);
        const double eq   = channelFolded (factor, false);
        const double comp = channelFolded (factor, true);
        CAPTURE (factor, eq, comp);
        REQUIRE (strip.getOversamplingLatencySamples() == expectedLatency (factor));
        REQUIRE (eq - eqNative < -30.0);
        REQUIRE (comp - compNative < -15.0);
    }

    ChannelStrip native;
    duskstudio::ChannelStripParams params;
    native.bind (params);
    native.prepare (kSr, kBlock, 1);
    REQUIRE (native.getOversamplingLatencySamples() == 0);
}

TEST_CASE ("Effect oversampling raises the bus compressor rate", "[oversampling]")
{
    const double native = busFolded (1);
    CAPTURE (native);
    REQUIRE (native > -90.0);

    for (const int factor : { 1, 2, 4 })
    {
        duskstudio::BusStrip strip;
        strip.prepare (kSr, kBlock, factor);
        REQUIRE (strip.getOversamplingLatencySamples() == expectedLatency (factor));
        if (factor == 1) continue;
        const double folded = busFolded (factor);
        CAPTURE (factor, folded);
        REQUIRE (folded - native < -30.0);
    }
}

TEST_CASE ("Effect oversampling raises the master EQ and compressor rate", "[oversampling]")
{
    const bool eq = GENERATE (true, false);
    const double native = masterFolded (1, eq, ! eq);
    CAPTURE (eq, native);
    REQUIRE (native > -90.0);

    for (const int factor : { 1, 2, 4 })
    {
        duskstudio::MasterBus master;
        master.prepare (kSr, kBlock, factor);
        REQUIRE (master.getOversamplingLatencySamples() == expectedLatency (factor));
        if (factor == 1) continue;
        const double folded = masterFolded (factor, eq, ! eq);
        CAPTURE (factor, folded);
        REQUIRE (folded - native < -30.0);
    }
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/BusStrip.h"
#include "dsp/ChannelStrip.h"
#include "dsp/MasterBus.h"
#include "dsp/MasteringChain.h"
#include "dsp/MultiCompStage.h"
#include "engine/builtin/DafPlugin.h"
#include "session/Session.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

using Catch::Matchers::WithinAbs;
using duskstudio::ChannelStrip;

// Effect Oversampling raises the internal rate of the channel EQ and the
// master EQ. Each saturates, and a stage that saturates a 15 kHz tone at
// 48 kHz folds its 2nd and 3rd harmonics (30 and 45 kHz) back to 18 and 3 kHz.
// Run at 2x or 4x, those harmonics sit below the raised Nyquist and the
// downsampler removes them, so the folded tones fall away. That drop is what
// shows the factor reached the stage, and the unit's own oversampler's
// reported latency that it took the factor. The compressors are Multi-Comp 2
// at its own 2x whatever the factor, which they read back.

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

double channelEqFolded (int factor)
{
    duskstudio::ChannelStripParams params;
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

double masterEqFolded (int factor)
{
    duskstudio::MasterBusParams params;
    params.eqEnabled.store (true);
    duskstudio::MasterBus master;
    master.bind (params);
    master.prepare (kSr, kBlock, factor);
    return foldedDb (probe ([&] (float* l, float* r, int n) { master.processInPlace (l, r, n); }, 0.5f));
}

int oversamplingIndex (duskstudio::MultiCompStage& stage)
{
    const auto& descs = stage.plugin().params();
    for (std::size_t i = 0; i < descs.size(); ++i)
        if (descs[i].symbol == "oversampling")
            return (int) i;
    return -1;
}

// The compressor's plug-in runs its own default Oversampling, 2x (parameter
// value 1), once a block has pushed the stage's values.
void checkOwnOversampling (duskstudio::MultiCompStage& stage)
{
    const int index = oversamplingIndex (stage);
    REQUIRE (index >= 0);
    const float setting = stage.plugin().getParameterValue ((std::uint32_t) index);
    CHECK_THAT (setting, WithinAbs (stage.plugin().params()[(std::size_t) index].defaultValue, 0.0f));
    CHECK_THAT (setting, WithinAbs (1.0f, 0.0f));
}

// The half-band round trip: 23 samples at 2x, 26.5 rounded at 4x.
int expectedLatency (int factor)
{
    return factor == 4 ? 27 : (factor == 2 ? 23 : 0);
}
} // namespace

TEST_CASE ("Effect oversampling raises the channel EQ rate", "[oversampling]")
{
    // The EQ core's console saturator always runs, so the probe hears the EQ
    // stage.
    const double native = channelEqFolded (1);
    CAPTURE (native);
    REQUIRE (native > -90.0);

    for (const int factor : { 1, 2, 4 })
    {
        CAPTURE (factor);
        ChannelStrip strip;
        duskstudio::ChannelStripParams params;
        strip.bind (params);
        strip.prepare (kSr, kBlock, factor);
        REQUIRE (strip.getOversamplingLatencySamples() == expectedLatency (factor));
        if (factor == 1) continue;
        const double folded = channelEqFolded (factor);
        CAPTURE (folded);
        REQUIRE (folded - native < -30.0);
    }
}

TEST_CASE ("Effect oversampling raises the master EQ rate", "[oversampling]")
{
    const double native = masterEqFolded (1);
    CAPTURE (native);
    REQUIRE (native > -90.0);

    for (const int factor : { 1, 2, 4 })
    {
        CAPTURE (factor);
        duskstudio::MasterBus master;
        master.prepare (kSr, kBlock, factor);
        REQUIRE (master.getOversamplingLatencySamples() == expectedLatency (factor));
        if (factor == 1) continue;
        const double folded = masterEqFolded (factor);
        CAPTURE (folded);
        REQUIRE (folded - native < -30.0);
    }
}

TEST_CASE ("Effect oversampling leaves every compressor at Multi-Comp 2's own 2x", "[oversampling]")
{
    // Multi-Comp 2's reference parity was established at its default 2x, so
    // every compressor runs that whatever the strip, bus or master was
    // prepared with, and keeps one latency.
    std::vector<float> l ((std::size_t) kBlock, 0.1f), r ((std::size_t) kBlock, 0.1f);
    std::array<float*, ChannelStrip::kNumBuses> busL {}, busR {};
    std::array<float*, ChannelStrip::kNumAuxSends> auxL {}, auxR {};
    std::vector<float> mL ((std::size_t) kBlock), mR ((std::size_t) kBlock);
    juce::MidiBuffer midi;
    int latency = -1;
    for (const int factor : { 1, 2, 4 })
    {
        CAPTURE (factor);
        // Each compressor runs inside its stage, held on as an engaged one
        // holds it.
        duskstudio::CompStageClock stage;
        const auto hold = [&stage] (int compLatency)
        {
            stage.prepare (kSr, compLatency);
            stage.snap (true);
            return &stage.block();
        };

        duskstudio::ChannelStripParams stripParams;
        stripParams.compEnabled.store (true);
        ChannelStrip strip;
        strip.bind (stripParams);
        strip.insertMode.store (ChannelStrip::kInsertEmpty);
        strip.prepare (kSr, kBlock, factor);
        strip.setCompStage (hold (strip.getCompressor (true).latencySamples()));
        for (const bool stereo : { false, true })
            strip.processAndAccumulate (l.data(), stereo ? r.data() : nullptr, midi, false,
                                        mL.data(), mR.data(), busL, busR, auxL, auxR, kBlock, true);
        checkOwnOversampling (strip.getCompressor (false));
        checkOwnOversampling (strip.getCompressor (true));

        duskstudio::BusParams busParams;
        busParams.compEnabled.store (true);
        duskstudio::BusStrip bus;
        bus.bind (busParams);
        bus.prepare (kSr, kBlock);
        bus.setCompStage (hold (bus.getCompressor().latencySamples()));
        bus.processInPlace (l.data(), r.data(), kBlock);
        checkOwnOversampling (bus.getCompressor());

        duskstudio::MasterBusParams masterParams;
        masterParams.compEnabled.store (true);
        duskstudio::MasterBus master;
        master.bind (masterParams);
        master.prepare (kSr, kBlock, factor);
        master.setCompStage (hold (master.getCompressor().latencySamples()));
        master.processInPlace (l.data(), r.data(), kBlock);
        checkOwnOversampling (master.getCompressor());

        // The mastering chain takes no factor at all.
        duskstudio::MasteringParams masteringParams;
        masteringParams.compEnabled.store (true);
        duskstudio::MasteringChain mastering;
        mastering.bind (masteringParams);
        mastering.prepare (kSr, kBlock);
        mastering.processInPlace (l.data(), r.data(), kBlock);
        checkOwnOversampling (mastering.getCompressor());

        if (latency < 0) latency = master.getCompressor().latencySamples();
        CHECK (strip.getCompressor (false).latencySamples() == latency);
        CHECK (strip.getCompressor (true).latencySamples() == latency);
        CHECK (master.getCompressor().latencySamples() == latency);
        CHECK (mastering.getCompressor().latencySamples() == latency);
    }
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/BusStrip.h"
#include "dsp/ChannelStrip.h"
#include "dsp/CompStageAlign.h"
#include "dsp/MasterBus.h"
#include "session/Session.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

using duskstudio::BusParams;
using duskstudio::BusStrip;
using duskstudio::ChannelStrip;
using duskstudio::ChannelStripParams;
using duskstudio::CompParamId;
using duskstudio::CompStageClock;
using duskstudio::MasterBus;
using duskstudio::MasterBusParams;

// Each mixer stage's members carry the stage's latency together, engaged or
// not, and with the stage off a strip, bus or master plays exactly as one
// with no stage at all.
namespace
{
constexpr double kSr    = 48000.0;
constexpr int    kBlock = 256;

struct StripRig
{
    ChannelStripParams params;
    ChannelStrip strip;
    std::vector<float> in, masterL, masterR;
    std::array<std::vector<float>, ChannelStrip::kNumBuses>    busL, busR;
    std::array<std::vector<float>, ChannelStrip::kNumAuxSends> auxL, auxR;
    std::array<float*, ChannelStrip::kNumBuses>    busLPtrs {}, busRPtrs {};
    std::array<float*, ChannelStrip::kNumAuxSends> auxLPtrs {}, auxRPtrs {};

    explicit StripRig (const duskstudio::CompStageBlock* stage)
    {
        for (auto* v : { &in, &masterL, &masterR })
            v->assign ((std::size_t) kBlock, 0.0f);
        for (std::size_t i = 0; i < busL.size(); ++i)
        {
            busL[i].assign ((std::size_t) kBlock, 0.0f);
            busR[i].assign ((std::size_t) kBlock, 0.0f);
            busLPtrs[i] = busL[i].data();
            busRPtrs[i] = busR[i].data();
        }
        for (std::size_t i = 0; i < auxL.size(); ++i)
        {
            auxL[i].assign ((std::size_t) kBlock, 0.0f);
            auxR[i].assign ((std::size_t) kBlock, 0.0f);
            auxLPtrs[i] = auxL[i].data();
            auxRPtrs[i] = auxR[i].data();
        }
        params.compMode.store (2);
        // The VCA with its threshold at the top of its range leaves a quiet
        // impulse alone.
        params.comp.set (CompParamId::VcaThreshold, 0.0f);
        strip.bind (params);
        strip.insertMode.store (ChannelStrip::kInsertEmpty);
        strip.setCompStage (stage);
        strip.prepare (kSr, kBlock);
    }

    // One block of `in`, mono; the master's left side comes back.
    const std::vector<float>& block()
    {
        std::fill (masterL.begin(), masterL.end(), 0.0f);
        std::fill (masterR.begin(), masterR.end(), 0.0f);
        juce::MidiBuffer midi;
        strip.processAndAccumulate (in.data(), nullptr, midi, false, masterL.data(), masterR.data(),
                                    busLPtrs, busRPtrs, auxLPtrs, auxRPtrs, kBlock, true);
        return masterL;
    }
};

std::vector<float> impulseBlocks (int blocks, int at, float amp)
{
    std::vector<float> v ((std::size_t) (blocks * kBlock), 0.0f);
    v[(std::size_t) at] = amp;
    return v;
}

int peakAt (const std::vector<float>& v)
{
    std::size_t best = 0;
    for (std::size_t i = 0; i < v.size(); ++i)
        if (std::abs (v[i]) > std::abs (v[best])) best = i;
    return (int) best;
}

int stripLatency()
{
    ChannelStrip probe;
    ChannelStripParams params;
    probe.bind (params);
    probe.prepare (kSr, kBlock);
    return probe.getCompressor (false).latencySamples();
}
} // namespace

TEST_CASE ("Comp stage: two strips, one compressor in, land on the same sample", "[comp-stage][channel-strip]")
{
    const int latency = stripLatency();
    REQUIRE (latency > 0);
    CompStageClock clock;
    clock.prepare (kSr, latency);
    clock.snap (true);

    StripRig engaged (&clock.block()), plain (&clock.block());
    engaged.params.compEnabled.store (true);
    const auto input = impulseBlocks (24, 12 * kBlock + 7, 0.05f);
    std::vector<float> outEngaged, outPlain;
    for (int b = 0; b < 24; ++b)
    {
        clock.advance (true, false, kBlock);
        std::copy_n (input.begin() + b * kBlock, kBlock, engaged.in.begin());
        std::copy_n (input.begin() + b * kBlock, kBlock, plain.in.begin());
        const auto& e = engaged.block();
        outEngaged.insert (outEngaged.end(), e.begin(), e.end());
        const auto& p = plain.block();
        outPlain.insert (outPlain.end(), p.begin(), p.end());
    }
    CHECK (peakAt (outPlain) == 12 * kBlock + 7 + latency);
    CHECK (peakAt (outEngaged) == peakAt (outPlain));
    CHECK (engaged.strip.getCurrentGrDb() <= 0.0f);
}

TEST_CASE ("Comp stage: a strip whose stage is off plays as one with no stage", "[comp-stage][channel-strip]")
{
    for (const bool stereo : { false, true })
    {
        CAPTURE (stereo);
        CompStageClock clock;
        clock.prepare (kSr, stripLatency());
        clock.snap (false);
        StripRig staged (&clock.block()), bare (nullptr);
        for (auto* rig : { &staged, &bare })
        {
            rig->params.eqEnabled.store (true);
            rig->params.lmGainDb.store (6.0f);
        }
        std::vector<float> right ((std::size_t) kBlock);
        bool same = true;
        for (int b = 0; b < 40; ++b)
        {
            clock.advance (false, true, kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                const float s = 0.3f * (float) std::sin (0.031 * (double) (b * kBlock + i));
                staged.in[(std::size_t) i] = bare.in[(std::size_t) i] = s;
                right[(std::size_t) i] = -0.5f * s;
            }
            for (auto* rig : { &staged, &bare })
            {
                std::fill (rig->masterL.begin(), rig->masterL.end(), 0.0f);
                std::fill (rig->masterR.begin(), rig->masterR.end(), 0.0f);
                juce::MidiBuffer midi;
                rig->strip.processAndAccumulate (rig->in.data(), stereo ? right.data() : nullptr, midi, false,
                                                 rig->masterL.data(), rig->masterR.data(),
                                                 rig->busLPtrs, rig->busRPtrs, rig->auxLPtrs, rig->auxRPtrs,
                                                 kBlock, true);
            }
            same = same && staged.masterL == bare.masterL && staged.masterR == bare.masterR;
        }
        CHECK (same);
    }
}

namespace
{
int busLatency()
{
    BusStrip probe;
    BusParams params;
    probe.bind (params);
    probe.prepare (kSr, kBlock);
    return probe.getCompressor().latencySamples();
}

// A bus compressor that leaves a quiet signal alone.
void transparentBusComp (BusParams& params)
{
    params.comp.set (CompParamId::BusThreshold, 15.0f);
}
} // namespace

TEST_CASE ("Comp stage: two buses, one compressor in, land on the same sample", "[comp-stage][BusStrip]")
{
    const int latency = busLatency();
    CompStageClock clock;
    clock.prepare (kSr, latency);
    clock.snap (true);
    BusParams engagedParams, plainParams;
    transparentBusComp (engagedParams);
    engagedParams.compEnabled.store (true);
    BusStrip engaged, plain;
    engaged.bind (engagedParams);
    plain.bind (plainParams);
    for (auto* bus : { &engaged, &plain })
    {
        bus->setCompStage (&clock.block());
        bus->prepare (kSr, kBlock);
    }

    const int at = 5 * kBlock + 100;
    auto eL = impulseBlocks (12, at, 0.05f), eR = eL, pL = eL, pR = eL;
    for (int b = 0; b < 12; ++b)
    {
        clock.advance (true, false, kBlock);
        engaged.processInPlace (eL.data() + b * kBlock, eR.data() + b * kBlock, kBlock);
        plain.processInPlace (pL.data() + b * kBlock, pR.data() + b * kBlock, kBlock);
    }
    CHECK (peakAt (pL) == at + latency);
    CHECK (peakAt (eL) == at + latency);
    CHECK (peakAt (eR) == at + latency);
}

TEST_CASE ("Comp stage: a bus switched in and out while playing keeps its timing", "[comp-stage][BusStrip]")
{
    // Each click after a switch and its fade lands a latency late, as the
    // first one did before anything was switched.
    const int latency = busLatency();
    CompStageClock clock;
    clock.prepare (kSr, latency);
    clock.snap (false);
    BusParams params;
    transparentBusComp (params);
    BusStrip bus;
    bus.bind (params);
    bus.setCompStage (&clock.block());
    bus.prepare (kSr, kBlock);

    // Clicks at the start of blocks 30, 60 and 90; engaged at 10, out at 40,
    // in again at 70. The transport rolls throughout.
    std::vector<float> L ((std::size_t) (100 * kBlock), 0.0f);
    for (const int b : { 30, 60, 90 }) L[(std::size_t) (b * kBlock + 3)] = 0.05f;
    auto R = L;
    for (int b = 0; b < 100; ++b)
    {
        params.compEnabled.store ((b >= 10 && b < 40) || b >= 70);
        clock.advance (params.compEnabled.load(), false, kBlock);
        bus.processInPlace (L.data() + b * kBlock, R.data() + b * kBlock, kBlock);
    }
    for (const int b : { 30, 60, 90 })
    {
        CAPTURE (b);
        const std::vector<float> window (L.begin() + b * kBlock, L.begin() + (b + 2) * kBlock);
        CHECK (peakAt (window) == 3 + latency);
    }
    CHECK (clock.reportedLatencySamples() == latency);
}

TEST_CASE ("Comp stage: a bus whose stage is off plays as one with no stage", "[comp-stage][BusStrip]")
{
    CompStageClock clock;
    clock.prepare (kSr, busLatency());
    clock.snap (false);
    BusParams params;
    params.eqEnabled.store (true);
    params.eqMidGainDb.store (4.0f);
    BusStrip staged, bare;
    for (auto* bus : { &staged, &bare })
    {
        bus->bind (params);
        bus->prepare (kSr, kBlock);
    }
    staged.setCompStage (&clock.block());
    bool same = true;
    std::vector<float> sL ((std::size_t) kBlock), sR (sL), bL (sL), bR (sL);
    for (int b = 0; b < 40; ++b)
    {
        clock.advance (false, true, kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            const float s = 0.4f * (float) std::sin (0.07 * (double) (b * kBlock + i));
            sL[(std::size_t) i] = bL[(std::size_t) i] = s;
            sR[(std::size_t) i] = bR[(std::size_t) i] = -s;
        }
        staged.processInPlace (sL.data(), sR.data(), kBlock);
        bare.processInPlace (bL.data(), bR.data(), kBlock);
        same = same && sL == bL && sR == bR;
    }
    CHECK (same);
}

namespace
{
void quietTape (MasterBusParams& params)
{
    params.tapeEnabled.store (false);
    params.tape.autoCal.store (false);
    params.tape.autoComp.store (false);
    params.tape.noiseAmount.store (0.0f);
    params.tape.wow.store (0.0f);
    params.tape.flutter.store (0.0f);
}
} // namespace

TEST_CASE ("Comp stage: the master's compressor in or out leaves its timing alone while the stage holds", "[comp-stage][master]")
{
    MasterBus probe;
    MasterBusParams probeParams;
    probe.bind (probeParams);
    probe.prepare (kSr, kBlock, 1);
    const int latency = probe.getCompressor().latencySamples();

    int position[2] {};
    for (const bool engaged : { false, true })
    {
        CompStageClock clock;
        clock.prepare (kSr, latency);
        clock.snap (true);
        MasterBusParams params;
        quietTape (params);
        params.comp.set (CompParamId::BusThreshold, 15.0f);
        params.compEnabled.store (engaged);
        MasterBus master;
        master.bind (params);
        master.setCompStage (&clock.block());
        master.prepare (kSr, kBlock, 1);
        auto L = impulseBlocks (16, 6 * kBlock + 11, 0.05f), R = L;
        for (int b = 0; b < 16; ++b)
        {
            clock.advance (true, false, kBlock);
            master.processInPlace (L.data() + b * kBlock, R.data() + b * kBlock, kBlock);
        }
        position[engaged ? 1 : 0] = peakAt (L);
    }
    CHECK (position[0] == 6 * kBlock + 11 + latency + probe.getTapeLatencySamples());
    CHECK (position[1] == position[0]);
}

TEST_CASE ("Comp stage: a master whose stage is off plays as one with no stage", "[comp-stage][master]")
{
    CompStageClock clock;
    MasterBusParams params;
    quietTape (params);
    params.eqEnabled.store (true);
    params.eqLfBoost.store (4.0f);
    MasterBus staged, bare;
    for (auto* master : { &staged, &bare })
    {
        master->bind (params);
        master->prepare (kSr, kBlock, 2);
    }
    clock.prepare (kSr, staged.getCompressor().latencySamples());
    clock.snap (false);
    staged.setCompStage (&clock.block());
    bool same = true;
    std::vector<float> sL ((std::size_t) kBlock), sR (sL), bL (sL), bR (sL);
    for (int b = 0; b < 40; ++b)
    {
        clock.advance (false, true, kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            const float s = 0.4f * (float) std::sin (0.05 * (double) (b * kBlock + i));
            sL[(std::size_t) i] = bL[(std::size_t) i] = s;
            sR[(std::size_t) i] = bR[(std::size_t) i] = 0.5f * s;
        }
        staged.processInPlace (sL.data(), sR.data(), kBlock);
        bare.processInPlace (bL.data(), bR.data(), kBlock);
        same = same && sL == bL && sR == bR;
    }
    CHECK (same);
}

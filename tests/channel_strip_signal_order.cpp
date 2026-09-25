#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/ChannelStrip.h"
#include "session/Session.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

using Catch::Matchers::WithinAbs;
using duskstudio::ChannelStrip;

// MANUAL "The channel strip" fixes the order phase -> insert -> HPF -> EQ ->
// LPF -> compressor -> pan -> fader -> routing. Each case below pins one
// boundary with a probe whose result would change if the two stages swapped.
// Two pairs have no observable order and are not probed: the HPF, the four
// bands and the LPF are a linear cascade ahead of the core's console
// saturator, and pan and fader are both plain gains, so either pair gives the
// same output in any order.

namespace
{
constexpr double kSr     = 48000.0;
constexpr int    kBlock  = 512;
constexpr double kTwoPi  = 6.283185307179586;
// ~430 ms: past the 20 ms smoothers, the insert crossfade gate and the
// compressor's attack, so the measured blocks are steady state.
constexpr int kSettleBlocks  = 40;
constexpr int kMeasureBlocks = 8;

struct Tone
{
    double hz    = 1000.0;
    float  amp   = 0.25f;
    double phase = 0.0;

    void fill (float* dst, int n)
    {
        const double inc = kTwoPi * hz / kSr;
        for (int i = 0; i < n; ++i)
        {
            dst[i] = amp * (float) std::sin (phase);
            phase += inc;
        }
    }
};

double rmsDb (const std::vector<float>& v)
{
    double sum = 0.0;
    for (const float x : v) sum += (double) x * (double) x;
    const double r = std::sqrt (sum / (double) std::max<std::size_t> (1, v.size()));
    return 20.0 * std::log10 (std::max (r, 1.0e-12));
}

// A mono strip with its outboard insert wired to device outputs 0/1 and
// inputs 0/1 at zero latency, fully wet. The test decides what the outboard
// returns, which makes the insert's input (the send) and its output (the
// return) separately observable.
struct Rig
{
    duskstudio::ChannelStripParams   params;
    duskstudio::HardwareInsertParams hw;
    ChannelStrip strip;

    std::vector<float> in, masterL, masterR, sendL, sendR, returnL, returnR;
    std::array<std::vector<float>, ChannelStrip::kNumBuses>    busL, busR;
    std::array<std::vector<float>, ChannelStrip::kNumAuxSends> auxL, auxR;
    std::array<float*, ChannelStrip::kNumBuses>    busLPtrs {}, busRPtrs {};
    std::array<float*, ChannelStrip::kNumAuxSends> auxLPtrs {}, auxRPtrs {};

    // What was captured over the measured blocks.
    std::vector<float> capIn, capSend, capMaster, capMasterR, capBus;

    explicit Rig (int insertMode)
    {
        for (auto* v : { &in, &masterL, &masterR, &sendL, &sendR, &returnL, &returnR })
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

        hw.enabled.store (true);
        hw.routing.publish (std::make_unique<duskstudio::HardwareInsertRouting> (
            duskstudio::HardwareInsertRouting { 0, 1, 0, 1, 0, 0 }));

        strip.bind (params);
        strip.bindHardwareInsert (hw);
        strip.insertMode.store (insertMode, std::memory_order_release);
    }

    void prepare() { strip.prepare (kSr, kBlock); }

    // `returnTone` null: the outboard is a wire, returning exactly what it was
    // sent in the same block.
    void run (Tone input, Tone* returnTone = nullptr)
    {
        prepare();
        capIn.clear(); capSend.clear(); capMaster.clear(); capMasterR.clear(); capBus.clear();

        const float* devIn[]  = { returnL.data(), returnR.data() };
        float*       devOut[] = { sendL.data(),   sendR.data() };
        juce::MidiBuffer midi;

        for (int b = 0; b < kSettleBlocks + kMeasureBlocks; ++b)
        {
            input.fill (in.data(), kBlock);
            if (returnTone != nullptr)
            {
                returnTone->fill (returnL.data(), kBlock);
                returnR = returnL;
            }
            else
            {
                returnL = in;
                returnR = in;
            }
            for (auto* v : { &masterL, &masterR, &sendL, &sendR })
                std::fill (v->begin(), v->end(), 0.0f);
            for (auto& v : busL) std::fill (v.begin(), v.end(), 0.0f);
            for (auto& v : busR) std::fill (v.begin(), v.end(), 0.0f);
            for (auto& v : auxL) std::fill (v.begin(), v.end(), 0.0f);
            for (auto& v : auxR) std::fill (v.begin(), v.end(), 0.0f);

            strip.processAndAccumulate (in.data(), nullptr, midi, false,
                                        masterL.data(), masterR.data(),
                                        busLPtrs, busRPtrs, auxLPtrs, auxRPtrs,
                                        kBlock, true, devIn, 2, devOut, 2);

            if (b >= kSettleBlocks)
            {
                capIn     .insert (capIn.end(),      in.begin(),      in.end());
                capSend   .insert (capSend.end(),    sendL.begin(),   sendL.end());
                capMaster .insert (capMaster.end(),  masterL.begin(), masterL.end());
                capMasterR.insert (capMasterR.end(), masterR.begin(), masterR.end());
                capBus    .insert (capBus.end(),     busL[0].begin(), busL[0].end());
            }
        }
    }

    void compressHard()
    {
        // VCA at the bottom of its threshold range, 8:1, so any tone the tests
        // use sits well into gain reduction.
        params.compEnabled.store (true);
        params.compMode.store (2);
        params.compVcaThreshDb.store (-38.0f);
        params.compVcaRatio.store (8.0f);
    }
};
} // namespace

TEST_CASE ("channel strip order: phase invert comes before the insert", "[channel-strip][order]")
{
    // The outboard returns a 3 kHz tone of its own, unrelated to what it is
    // sent. If Ø acts before the insert, it flips the send and leaves the
    // return alone; if it acted after, the send would keep its polarity and
    // the strip's output would flip.
    Tone input { 1000.0, 0.25f };
    Tone outboard { 3000.0, 0.25f };

    Rig normal { ChannelStrip::kInsertHardware };
    Tone outboardA = outboard;
    normal.run (input, &outboardA);

    Rig flipped { ChannelStrip::kInsertHardware };
    flipped.params.phaseInvert.store (true);
    Tone outboardB = outboard;
    flipped.run (input, &outboardB);

    // Largest sample difference between a and b, with b scaled by `sign`.
    auto maxDiff = [] (const std::vector<float>& a, const std::vector<float>& b, float sign)
    {
        double worst = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i)
            worst = std::max (worst, (double) std::abs (a[i] - sign * b[i]));
        return worst;
    };

    REQUIRE (normal.capSend.size() == normal.capIn.size());
    REQUIRE (rmsDb (normal.capSend) > -20.0);
    REQUIRE (maxDiff (normal.capSend,  normal.capIn,   1.0f) < 1.0e-6);
    REQUIRE (maxDiff (flipped.capSend, flipped.capIn, -1.0f) < 1.0e-6);

    REQUIRE (rmsDb (normal.capMaster) > -30.0);
    REQUIRE (maxDiff (flipped.capMaster, normal.capMaster, 1.0f) < 1.0e-6);
}

TEST_CASE ("channel strip order: the insert comes before HPF / EQ / LPF", "[channel-strip][order]")
{
    // The outboard is a wire. Whatever the tone section does, the send carries
    // the untouched input, so the insert sits ahead of it; and the strip's
    // output does move, so the section acts on what the insert returned.
    auto check = [] (double hz, auto&& engage, double minChangeDb)
    {
        Rig flat { ChannelStrip::kInsertHardware };
        flat.run (Tone { hz, 0.25f });

        Rig shaped { ChannelStrip::kInsertHardware };
        shaped.params.eqEnabled.store (true);
        engage (shaped.params);
        shaped.run (Tone { hz, 0.25f });

        CAPTURE (hz, rmsDb (shaped.capSend), rmsDb (shaped.capIn),
                 rmsDb (shaped.capMaster), rmsDb (flat.capMaster));
        REQUIRE_THAT (rmsDb (shaped.capSend), WithinAbs (rmsDb (shaped.capIn), 0.01));
        REQUIRE (std::abs (rmsDb (shaped.capMaster) - rmsDb (flat.capMaster)) > minChangeDb);
    };

    SECTION ("HPF")
    {
        check (50.0, [] (duskstudio::ChannelStripParams& p)
        {
            p.hpfEnabled.store (true);
            p.hpfFreq.store (300.0f);
        }, 12.0);
    }
    SECTION ("EQ band")
    {
        check (1000.0, [] (duskstudio::ChannelStripParams& p)
        {
            p.lmFreq.store (1000.0f);
            p.lmGainDb.store (12.0f);
        }, 6.0);
    }
    SECTION ("LPF")
    {
        check (12000.0, [] (duskstudio::ChannelStripParams& p)
        {
            p.lpfEnabled.store (true);
            p.lpfFreq.store (3000.0f);
        }, 12.0);
    }
}

TEST_CASE ("channel strip order: HPF / EQ / LPF come before the compressor", "[channel-strip][order]")
{
    // With the compressor deep in gain reduction, filtering the tone away or
    // cutting it must shrink the reduction. A tone section placed after the
    // compressor could change the output level but never what the detector
    // heard.
    auto grDb = [] (double hz, auto&& engage)
    {
        Rig rig { ChannelStrip::kInsertEmpty };
        rig.compressHard();
        rig.params.eqEnabled.store (true);
        engage (rig.params);
        rig.run (Tone { hz, 0.5f });
        return (double) rig.strip.getCurrentGrDb();
    };
    auto none = [] (duskstudio::ChannelStripParams&) {};

    SECTION ("HPF")
    {
        const double open = grDb (100.0, none);
        const double cut  = grDb (100.0, [] (duskstudio::ChannelStripParams& p)
        {
            p.hpfEnabled.store (true);
            p.hpfFreq.store (300.0f);
        });
        CAPTURE (open, cut);
        REQUIRE (open < -6.0);
        REQUIRE (cut - open > 6.0);
    }
    SECTION ("EQ band")
    {
        const double open = grDb (1000.0, none);
        const double cut  = grDb (1000.0, [] (duskstudio::ChannelStripParams& p)
        {
            p.lmFreq.store (1000.0f);
            p.lmGainDb.store (-12.0f);
        });
        CAPTURE (open, cut);
        REQUIRE (open < -6.0);
        REQUIRE (cut - open > 6.0);
    }
    SECTION ("LPF")
    {
        const double open = grDb (12000.0, none);
        const double cut  = grDb (12000.0, [] (duskstudio::ChannelStripParams& p)
        {
            p.lpfEnabled.store (true);
            p.lpfFreq.store (3000.0f);
        });
        CAPTURE (open, cut);
        REQUIRE (open < -6.0);
        REQUIRE (cut - open > 6.0);
    }
}

TEST_CASE ("channel strip order: the compressor comes before pan / fader / routing", "[channel-strip][order]")
{
    // The fader drops the output 20 dB and pan swings it hard left, yet the
    // gain reduction does not move: the detector hears the signal before
    // either. The bus receives the same faded, panned signal the master
    // would, so the master / bus split comes after both.
    const Tone tone { 1000.0, 0.5f };

    Rig centred { ChannelStrip::kInsertEmpty };
    centred.compressHard();
    centred.run (tone);

    Rig moved { ChannelStrip::kInsertEmpty };
    moved.compressHard();
    moved.params.liveFaderDb.store (-20.0f);
    moved.params.livePan.store (-1.0f);
    moved.run (tone);

    Rig routed { ChannelStrip::kInsertEmpty };
    routed.compressHard();
    routed.params.liveFaderDb.store (-20.0f);
    routed.params.livePan.store (-1.0f);
    routed.params.busAssign[0].store (true);
    routed.run (tone);

    const double grCentred = centred.strip.getCurrentGrDb();
    const double grMoved   = moved.strip.getCurrentGrDb();
    CAPTURE (grCentred, grMoved);
    REQUIRE (grCentred < -6.0);
    REQUIRE_THAT (grMoved, WithinAbs (grCentred, 0.1));

    // Equal-power pan at centre is -3 dB per side; hard left is 0 dB on L.
    REQUIRE_THAT (rmsDb (moved.capMaster) - rmsDb (centred.capMaster), WithinAbs (-20.0 + 3.01, 0.1));
    REQUIRE (rmsDb (moved.capMasterR) < -100.0);

    REQUIRE (rmsDb (routed.capMaster) < -100.0);
    REQUIRE_THAT (rmsDb (routed.capBus), WithinAbs (rmsDb (moved.capMaster), 0.01));
}

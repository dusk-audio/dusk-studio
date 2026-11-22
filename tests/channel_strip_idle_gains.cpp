#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/ChannelStrip.h"
#include "foundation/Decibels.h"
#include "foundation/SmoothedValue.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

// A strip that mixes nothing in a block (no input, or not passing) still ramps
// its fader and aux-send gains toward zero across that block, so the block it
// next mixes fades in from where the ramp got to. It moves each ramp on with
// one skip() per block rather than one getNextValue() per sample. Its bus
// gains are its route, which stays where it was, and which it takes at once
// whenever its fader gain is at zero. These cases hold that to what ticking
// every sample gives.

namespace
{
using Catch::Matchers::WithinAbs;
using duskstudio::ChannelStrip;
using duskstudio::ChannelStripParams;
using Ramp = dusk::audio::SmoothedValue<float>;

constexpr double kRate = 48000.0;
constexpr double kRampSeconds = 0.020;
constexpr int kRampSamples = 960;   // kRampSeconds at kRate
constexpr int kMaxBlock = 128;
constexpr int kBlock = 100;
constexpr float kEpsilon = std::numeric_limits<float>::epsilon();

float ulpOf (float x)
{
    const float a = std::abs (x);
    return std::nextafter (a, std::numeric_limits<float>::infinity()) - a;
}
} // namespace

// skip (n) adds step * n once where ticking adds step n times, rounding each
// time. Every rounding is at most half an ulp of the value the ramp left
// from. Skip's own multiply and add, and a step worked out afresh from the
// two slightly different values when a ramp restarts, add at most three ulp
// a block. So after `steps` samples taken as `skips` blocks the two differ by
// at most (steps / 2 + 3 * skips) ulp of that value. Once the ramp ends both
// hold its target exactly.
TEST_CASE ("Skipping a block of a gain ramp lands where ticking each sample does",
           "[channel-strip][smoothing]")
{
    // Start values: silence, an aux send at -10 dB, a bus at unity, an aux
    // send at its +6 dB top, the fader at its +12 dB top.
    const float start = GENERATE (0.0f, 0.31622777f, 1.0f, 1.9952623f, 3.9810717f);
    // Whether the strip goes idle with the gain settled or part way up a ramp.
    const bool midRamp = GENERATE (false, true);
    const int block = GENERATE (1, 37, 100, 512, 959, 960, 961);
    // Idle samples: inside the ramp, its last sample, past it, long past it.
    const int idleSamples = GENERATE (300, 959, 960, 1000, 6400);
    // A strip whose chain runs while it does not pass sets its targets from
    // its settings before it parks them, so each block starts a fresh ramp.
    const bool restartEachBlock = GENERATE (false, true);
    // What the gain heads for on wake: zero (a running ramp carries on) or a level.
    const float wakeTarget = GENERATE (0.0f, 0.5f, 3.9810717f);

    Ramp skipped, ticked;
    for (auto* r : { &skipped, &ticked })
    {
        r->reset (kRampSamples);
        r->setCurrentAndTargetValue (midRamp ? 0.0f : start);
        if (midRamp)
        {
            r->setTargetValue (2.0f * start);
            for (int i = 0; i < kRampSamples / 2; ++i)
                r->getNextValue();
        }
    }
    const float entry = ticked.getCurrentValue();
    REQUIRE_THAT (skipped.getCurrentValue(), WithinAbs (entry, 0.0));

    // Over the idle stretch: how far past its bound the skipped value strayed
    // (zero or less is inside), and the blocks the two disagreed on whether
    // they were still ramping.
    float idleExcess = -1.0f;
    int idleStateMismatches = 0, nonFinite = 0;
    int done = 0, skips = 0;
    while (done < idleSamples)
    {
        const int n = std::min (block, idleSamples - done);
        for (auto* r : { &skipped, &ticked })
        {
            if (restartEachBlock)
                r->setTargetValue (start);
            r->setTargetValue (0.0f);
        }
        skipped.skip (n);
        for (int i = 0; i < n; ++i)
            ticked.getNextValue();
        done += n;
        ++skips;

        if (skipped.isSmoothing() != ticked.isSmoothing())
            ++idleStateMismatches;
        const int steps = restartEachBlock ? done : std::min (done, kRampSamples);
        const float bound = ticked.isSmoothing()
            ? (0.5f * (float) steps + 3.0f * (float) skips) * ulpOf (entry)
            : 0.0f;
        idleExcess = std::max (idleExcess,
                               std::abs (skipped.getCurrentValue() - ticked.getCurrentValue()) - bound);
        nonFinite += std::isfinite (skipped.getCurrentValue()) && std::isfinite (ticked.getCurrentValue()) ? 0 : 1;
    }
    CHECK (idleStateMismatches == 0);
    CHECK (idleExcess <= 0.0f);

    // Wake: both now tick every sample. A ramp still running carries on with
    // the same countdown, and a new target starts a fresh ramp from where
    // each was; either way the gap can grow by about an ulp a sample at most,
    // and both reach the target on the same sample, exactly.
    const float idleGap = std::abs (skipped.getCurrentValue() - ticked.getCurrentValue());
    skipped.setTargetValue (wakeTarget);
    ticked.setTargetValue (wakeTarget);
    const float scale = std::max ({ std::abs (entry), std::abs (wakeTarget), 1.0f });
    float wakeExcess = -1.0f;
    int wakeStateMismatches = 0;
    for (int i = 0; i < 2 * kRampSamples; ++i)
    {
        const float s = skipped.getNextValue();
        const float t = ticked.getNextValue();
        if (skipped.isSmoothing() != ticked.isSmoothing())
            ++wakeStateMismatches;
        const float bound = ticked.isSmoothing() ? idleGap + (float) (i + 2) * ulpOf (scale) : 0.0f;
        wakeExcess = std::max (wakeExcess, std::abs (s - t) - bound);
        nonFinite += std::isfinite (s) && std::isfinite (t) ? 0 : 1;
    }
    CHECK (wakeStateMismatches == 0);
    CHECK (wakeExcess <= 0.0f);
    // std::max keeps its first argument against a NaN, so the excesses above cannot see one.
    CHECK (nonFinite == 0);
    REQUIRE_FALSE (ticked.isSmoothing());
    REQUIRE_THAT (skipped.getCurrentValue(), WithinAbs (wakeTarget, 0.0));
}

namespace
{
enum class Idle { noInput, notPassing, notPassingChainRuns };

// One strip setting, as the engine leaves it in the live atoms.
struct Mix
{
    float faderDb = 0.0f;
    bool  mute = false;
    float pan = 0.0f;
    bool  auxBypassed = false;
    std::array<bool, ChannelStrip::kNumBuses> bus {};
    std::array<float, ChannelStrip::kNumAuxSends> auxDb {
        ChannelStripParams::kAuxSendOffDb, ChannelStripParams::kAuxSendOffDb,
        ChannelStripParams::kAuxSendOffDb, ChannelStripParams::kAuxSendOffDb };
    std::array<bool, ChannelStrip::kNumAuxSends> auxPre {};

    // The same setting with every gain the idle paths ramp turned to zero by
    // hand: muted, sends bypassed. Pan and the route are left alone, as the
    // idle paths leave them.
    Mix turnedDown() const
    {
        Mix m = *this;
        m.mute = true;
        m.auxBypassed = true;
        return m;
    }
};

// When a setting changes around an idle stretch. `before` takes effect on the
// block ahead of the stretch, `during` half way through it, `wake` on the
// first block after it and `late` three blocks into the wake.
enum class Change { beforeStretch, duringStretch, turnedDownDuring, atAndAfterWake };

struct Timeline
{
    Mix initial, before, during, wake, late;
};

Timeline timelineFor (Change change)
{
    Timeline t;
    t.initial.faderDb = 0.0f;
    t.initial.pan = 0.3f;
    t.initial.bus = { true, false, true, false };
    t.initial.auxDb = { -6.0f, ChannelStripParams::kAuxSendOffDb, 0.0f, 6.0f };
    t.initial.auxPre = { true, false, false, true };
    t.before = t.during = t.wake = t.late = t.initial;
    switch (change)
    {
        case Change::beforeStretch:
            // Still ramping when the stretch starts.
            t.before.faderDb = ChannelStripParams::kFaderMaxDb;
            t.before.bus = { false, true, true, true };
            t.before.auxDb = { 6.0f, -3.0f, ChannelStripParams::kAuxSendOffDb, 0.0f };
            t.during = t.wake = t.late = t.before;
            break;
        case Change::duringStretch:
            t.during.faderDb = -12.0f;
            t.during.pan = -0.6f;
            t.during.bus = { false, true, false, true };
            t.during.auxDb = { ChannelStripParams::kAuxSendOffDb, 3.0f, -20.0f, 6.0f };
            t.during.auxPre = { false, true, false, true };
            t.wake = t.late = t.during;
            break;
        case Change::turnedDownDuring:
            // The ramps toward zero carry on through the wake, then the
            // setting comes back up.
            t.during.faderDb = ChannelStripParams::kFaderMinDb;
            t.during.bus.fill (false);
            t.during.auxDb.fill (ChannelStripParams::kAuxSendOffDb);
            t.wake = t.during;
            break;
        case Change::atAndAfterWake:
            t.wake.faderDb = 6.0f;
            t.wake.bus = { true, true, false, true };
            t.wake.auxDb = { 0.0f, -6.0f, 6.0f, ChannelStripParams::kAuxSendOffDb };
            t.late = t.wake;
            t.late.faderDb = -6.0f;
            t.late.pan = 0.5f;
            t.late.bus = { false, true, false, false };
            t.late.auxDb[2] = -12.0f;
            break;
    }
    return t;
}

constexpr int kSettleBlocks = 20;
// Three ramps' worth of wake, so every ramp the wake starts runs out.
constexpr int kWakeBlocks = 3 * kRampSamples / kBlock;
constexpr int kLateBlock = 3;

constexpr int kOutputs = 2 + 2 * ChannelStrip::kNumBuses + 2 * ChannelStrip::kNumAuxSends;
using Outputs = std::array<std::array<float, kMaxBlock>, kOutputs>;

float peakOf (const Outputs& out)
{
    float p = 0.0f;
    for (const auto& channel : out)
        for (int i = 0; i < kBlock; ++i)
            p = std::max (p, std::abs (channel[(std::size_t) i]));
    return p;
}

float worstGap (const Outputs& a, const Outputs& b)
{
    float w = 0.0f;
    for (std::size_t c = 0; c < a.size(); ++c)
        for (int i = 0; i < kBlock; ++i)
            w = std::max (w, std::abs (a[c][(std::size_t) i] - b[c][(std::size_t) i]));
    return w;
}

// Each output carries two ramped gains. Either can be off by a skipped
// stretch's roundings (half an ulp a sample over a ramp) plus a ramp of about
// one ulp a sample after it, so 4 * kRampSamples * epsilon of the loudest
// output bounds the pair.
float toleranceFor (float peak)
{
    return 4.0f * (float) kRampSamples * kEpsilon * peak;
}

// A stereo strip playing a frozen track through an empty insert: no EQ or
// compressor runs, so every output is its input times the strip's gains.
// Outputs are master L/R, then L/R of each bus, then L/R of each aux send.
struct Rig
{
    ChannelStripParams params;
    duskstudio::HardwareInsertParams hardware;
    ChannelStrip strip;
    juce::MidiBuffer midi;
    Outputs out {};
    // Output samples over every block that were NaN or infinite, which peakOf and
    // worstGap cannot see: std::max keeps its first argument against a NaN.
    int nonFinite = 0;
    std::array<float*, ChannelStrip::kNumBuses> busL {}, busR {};
    std::array<float*, ChannelStrip::kNumAuxSends> auxL {}, auxR {};

    Rig()
    {
        strip.bind (params);
        strip.bindHardwareInsert (hardware);
        strip.insertMode.store (ChannelStrip::kInsertEmpty, std::memory_order_release);
        strip.prepare (kRate, kMaxBlock);
        for (std::size_t a = 0; a < busL.size(); ++a)
        {
            busL[a] = out[2 + 2 * a].data();
            busR[a] = out[3 + 2 * a].data();
        }
        for (std::size_t a = 0; a < auxL.size(); ++a)
        {
            auxL[a] = out[2 + 2 * busL.size() + 2 * a].data();
            auxR[a] = out[3 + 2 * busL.size() + 2 * a].data();
        }
    }

    void apply (const Mix& m)
    {
        params.liveFaderDb.store (m.faderDb, std::memory_order_relaxed);
        params.liveMute.store (m.mute, std::memory_order_relaxed);
        params.livePan.store (m.pan, std::memory_order_relaxed);
        params.auxSendsBypassed.store (m.auxBypassed, std::memory_order_relaxed);
        for (std::size_t a = 0; a < m.bus.size(); ++a)
            params.busAssign[a].store (m.bus[a], std::memory_order_relaxed);
        for (std::size_t a = 0; a < m.auxDb.size(); ++a)
        {
            params.liveAuxSendDb[a].store (m.auxDb[a], std::memory_order_relaxed);
            params.auxSendPreFader[a].store (m.auxPre[a], std::memory_order_relaxed);
        }
    }

    void block (const float* inL, const float* inR, bool passByGate)
    {
        for (auto& channel : out)
            channel.fill (0.0f);
        strip.processAndAccumulate (inL, inR, midi, false,
                                    out[0].data(), out[1].data(), busL, busR, auxL, auxR,
                                    kBlock, passByGate, nullptr, 0, nullptr, 0, true);
        for (const auto& channel : out)
            for (int i = 0; i < kBlock; ++i)
                nonFinite += std::isfinite (channel[(std::size_t) i]) ? 0 : 1;
    }

    // One block on the idle path: the input withheld, or not passing.
    void idleBlock (Idle path, const float* inL, const float* inR)
    {
        switch (path)
        {
            case Idle::noInput:
                block (nullptr, nullptr, true);
                break;
            case Idle::notPassing:
                block (inL, inR, false);
                break;
            case Idle::notPassingChainRuns:
                strip.setNeedsProcessedMono (true);
                block (inL, inR, false);
                strip.setNeedsProcessedMono (false);
                break;
        }
    }
};

// A stereo test signal that never crosses zero, continuing across blocks.
struct Input
{
    long long sample = 0;
    std::array<float, kMaxBlock> l {}, r {};

    void next()
    {
        for (int i = 0; i < kBlock; ++i, ++sample)
        {
            const double n = (double) sample;
            l[(std::size_t) i] = 0.5f + 0.25f * (float) std::sin (6.283185307179586 * n / 97.0);
            r[(std::size_t) i] = -0.375f + 0.125f * (float) std::cos (6.283185307179586 * n / 61.0);
        }
    }
};

// The strip's gain stage with every ramp ticked one sample at a time: its
// targets (ChannelStrip::updateGainTargets), what an idle block does to them,
// and its accumulate loop.
struct GainModel
{
    Ramp fader { 0.0f }, panL { 0.7071f }, panR { 0.7071f };
    std::array<Ramp, ChannelStrip::kNumBuses> bus;
    std::array<Ramp, ChannelStrip::kNumAuxSends> aux;
    std::array<bool, ChannelStrip::kNumAuxSends> pre {};

    GainModel()
    {
        for (auto* r : { &fader, &panL, &panR })
            r->reset (kRate, kRampSeconds);
        for (auto& r : bus) { r.reset (kRate, kRampSeconds); r.setCurrentAndTargetValue (0.0f); }
        for (auto& r : aux) { r.reset (kRate, kRampSeconds); r.setCurrentAndTargetValue (0.0f); }
    }

    void follow (const Mix& m)
    {
        fader.setTargetValue ((m.mute || m.faderDb <= ChannelStripParams::kFaderInfThreshDb)
                              ? 0.0f : dusk::audio::decibelsToGain (m.faderDb));
        const float angle = (std::clamp (m.pan, -1.0f, 1.0f) + 1.0f) * (1.57079632679489661923f * 0.5f);
        panL.setTargetValue (std::cos (angle));
        panR.setTargetValue (std::sin (angle));
        const bool silent = fader.getCurrentValue() <= 0.0f;
        for (std::size_t a = 0; a < bus.size(); ++a)
        {
            if (silent) bus[a].setCurrentAndTargetValue (m.bus[a] ? 1.0f : 0.0f);
            else        bus[a].setTargetValue (m.bus[a] ? 1.0f : 0.0f);
        }
        for (std::size_t a = 0; a < aux.size(); ++a)
        {
            aux[a].setTargetValue ((m.auxBypassed || m.auxDb[a] <= ChannelStripParams::kAuxSendOffDb)
                                   ? 0.0f : dusk::audio::decibelsToGain (m.auxDb[a]));
            pre[a] = m.auxPre[a];
        }
    }

    void park (int n)
    {
        fader.setTargetValue (0.0f);
        for (auto& r : aux) r.setTargetValue (0.0f);
        for (int i = 0; i < n; ++i)
        {
            fader.getNextValue();
            for (auto& r : aux) r.getNextValue();
        }
    }

    void mix (const Input& in, Outputs& out)
    {
        for (auto& channel : out)
            channel.fill (0.0f);
        for (std::size_t i = 0; i < (std::size_t) kBlock; ++i)
        {
            const float fg = fader.getNextValue();
            const float pL = panL.getNextValue();
            const float pR = panR.getNextValue();
            const float wetLPre = in.l[i] * pL;
            const float wetRPre = in.r[i] * pR;
            const float wetL = in.l[i] * (pL * fg);
            const float wetR = in.r[i] * (pR * fg);
            std::array<float, ChannelStrip::kNumBuses> busG {};
            float maxBusG = 0.0f;
            for (std::size_t a = 0; a < bus.size(); ++a)
            {
                busG[a] = bus[a].getNextValue();
                maxBusG = std::max (maxBusG, busG[a]);
            }
            const float toMaster = std::max (0.0f, 1.0f - maxBusG);
            out[0][i] += wetL * toMaster;
            out[1][i] += wetR * toMaster;
            for (std::size_t a = 0; a < bus.size(); ++a)
                if (busG[a] > 0.0f)
                {
                    out[2 + 2 * a][i] += wetL * busG[a];
                    out[3 + 2 * a][i] += wetR * busG[a];
                }
            for (std::size_t a = 0; a < aux.size(); ++a)
            {
                const float sg = aux[a].getNextValue();
                if (sg <= 0.0f) continue;
                const std::size_t c = 2 + 2 * bus.size() + 2 * a;
                out[c][i]     += (pre[a] ? wetLPre : wetL) * sg;
                out[c + 1][i] += (pre[a] ? wetRPre : wetR) * sg;
            }
        }
    }
};
} // namespace

// The strip against the model, block by block from its first block: settling,
// the change ahead of the stretch, the stretch on one idle path, the wake.
// What the strip mixes is its gains at work, so matching the model on every
// output, through and after the stretch, holds each of the nine gains the idle
// paths move, and the ramp each is part way through, to the per-sample result.
TEST_CASE ("An idle strip's gains wake where ticking each sample leaves them",
           "[channel-strip][smoothing]")
{
    const Idle idlePath = GENERATE (Idle::noInput, Idle::notPassing, Idle::notPassingChainRuns);
    // 300 samples end inside the 960-sample ramp, 1000 cross its end inside
    // the last block, 6400 settle it long before the wake.
    const int stretchBlocks = GENERATE (3, 10, 64);
    const Change change = GENERATE (Change::beforeStretch, Change::duringStretch,
                                    Change::turnedDownDuring, Change::atAndAfterWake);
    const Timeline t = timelineFor (change);

    Rig rig;
    GainModel model;
    Input in;
    Outputs expected {};
    float peak = 0.0f, worst = 0.0f;
    const auto passing = [&] (const Mix& m)
    {
        rig.apply (m);
        in.next();
        rig.block (in.l.data(), in.r.data(), true);
        model.follow (m);
        model.mix (in, expected);
        peak = std::max (peak, peakOf (expected));
        worst = std::max (worst, worstGap (rig.out, expected));
    };

    for (int b = 0; b < kSettleBlocks; ++b)
        passing (t.initial);
    passing (t.before);
    for (int b = 0; b < stretchBlocks; ++b)
    {
        const Mix& m = b < stretchBlocks / 2 ? t.before : t.during;
        rig.apply (m);
        in.next();
        rig.idleBlock (idlePath, in.l.data(), in.r.data());
        // A strip whose chain runs reads its settings before the chain, then
        // parks the gains after it.
        if (idlePath == Idle::notPassingChainRuns)
            model.follow (m);
        model.park (kBlock);
        for (auto& channel : expected)
            channel.fill (0.0f);
        worst = std::max (worst, worstGap (rig.out, expected));
    }
    for (int b = 0; b < kWakeBlocks; ++b)
        passing (b < kLateBlock ? t.wake : t.late);

    REQUIRE (rig.nonFinite == 0);
    REQUIRE (peak > 0.1f);
    const float tolerance = toleranceFor (peak);
    INFO ("idle path " << (int) idlePath << ", " << stretchBlocks << " idle blocks, change "
          << (int) change << ", peak " << peak << ", worst " << worst << ", tolerance " << tolerance);
    REQUIRE_THAT (worst, WithinAbs (0.0, tolerance));
}

// Strip `idle` stops mixing for a stretch: it loses its input or stops
// passing. Strip `kept` mixes the whole time and has its fader and sends
// turned down by hand over the same stretch, so those gains ramp to zero one
// sample at a time. Once both mix again with the same settings, their outputs
// have to match, every channel and every sample. (A strip whose chain runs
// while it does not pass is left out: it restarts its ramps from its settings
// each block, as the case above models, so it fades slower than a strip
// turned down by hand. So is a route change still ramping when a stretch too
// short for the fader to reach zero starts: the idle strip holds it part made,
// the kept one finishes it.)
TEST_CASE ("A strip woken from an idle stretch mixes like one kept running with its gains down",
           "[channel-strip][smoothing]")
{
    const Idle idlePath = GENERATE (Idle::noInput, Idle::notPassing);
    const int stretchBlocks = GENERATE (3, 10, 64);
    const Change change = GENERATE (Change::beforeStretch, Change::duringStretch,
                                    Change::turnedDownDuring, Change::atAndAfterWake);
    if (change == Change::beforeStretch && stretchBlocks * kBlock < kRampSamples)
        return;
    const Timeline t = timelineFor (change);

    Rig idle, kept;
    Input in;
    const auto both = [&] (const Mix& m)
    {
        idle.apply (m);
        kept.apply (m);
        in.next();
        idle.block (in.l.data(), in.r.data(), true);
        kept.block (in.l.data(), in.r.data(), true);
    };

    for (int b = 0; b < kSettleBlocks; ++b)
        both (t.initial);
    both (t.before);

    float idleLeak = 0.0f;
    for (int b = 0; b < stretchBlocks; ++b)
    {
        idle.apply (b < stretchBlocks / 2 ? t.before : t.during);
        kept.apply (t.before.turnedDown());
        in.next();
        idle.idleBlock (idlePath, in.l.data(), in.r.data());
        kept.block (in.l.data(), in.r.data(), true);
        idleLeak = std::max (idleLeak, peakOf (idle.out));
    }
    REQUIRE_THAT (idleLeak, WithinAbs (0.0, 0.0));

    float peak = 0.0f, worst = 0.0f;
    for (int b = 0; b < kWakeBlocks; ++b)
    {
        both (b < kLateBlock ? t.wake : t.late);
        peak = std::max (peak, peakOf (kept.out));
        worst = std::max (worst, worstGap (idle.out, kept.out));
    }
    REQUIRE (idle.nonFinite == 0);
    REQUIRE (kept.nonFinite == 0);
    REQUIRE (peak > 0.1f);
    const float tolerance = toleranceFor (peak);
    INFO ("idle path " << (int) idlePath << ", " << stretchBlocks << " idle blocks, change "
          << (int) change << ", peak " << peak << ", worst " << worst << ", tolerance " << tolerance);
    REQUIRE_THAT (worst, WithinAbs (0.0, tolerance));
}

// The master gets what a strip's buses leave it, so a bus-routed strip coming
// back from an idle stretch, or mixing for the first time after prepare, must
// put nothing on the master while its fader fades it in: its route is the
// buses, and each bus it is on gets the whole fade.
TEST_CASE ("A bus-routed strip fades back in through its buses alone",
           "[channel-strip][smoothing]")
{
    const Idle idlePath = GENERATE (Idle::noInput, Idle::notPassing, Idle::notPassingChainRuns);
    const int stretchBlocks = GENERATE (0, 3, 10, 64);
    const bool severalBuses = GENERATE (false, true);
    Mix routed;
    routed.bus = severalBuses ? std::array<bool, ChannelStrip::kNumBuses> { true, false, true, false }
                              : std::array<bool, ChannelStrip::kNumBuses> { true, false, false, false };
    CAPTURE ((int) idlePath, stretchBlocks, severalBuses);

    Rig rig;
    GainModel model;
    Input in;
    Outputs expected {};
    float masterLeak = 0.0f, busPeak = 0.0f, worst = 0.0f;
    const auto passing = [&]
    {
        rig.apply (routed);
        in.next();
        rig.block (in.l.data(), in.r.data(), true);
        model.follow (routed);
        model.mix (in, expected);
        for (std::size_t c : { std::size_t { 0 }, std::size_t { 1 } })
            for (int i = 0; i < kBlock; ++i)
                masterLeak = std::max (masterLeak, std::abs (rig.out[c][(std::size_t) i]));
        for (std::size_t a = 0; a < (std::size_t) ChannelStrip::kNumBuses; ++a)
            for (int i = 0; i < kBlock; ++i)
                busPeak = std::max (busPeak, std::abs (rig.out[2 + 2 * a][(std::size_t) i]));
        worst = std::max (worst, worstGap (rig.out, expected));
    };

    // From prepare, then settled, then back from the stretch.
    for (int b = 0; b < kSettleBlocks; ++b)
        passing();
    for (int b = 0; b < stretchBlocks; ++b)
    {
        rig.apply (routed);
        in.next();
        rig.idleBlock (idlePath, in.l.data(), in.r.data());
        if (idlePath == Idle::notPassingChainRuns)
            model.follow (routed);
        model.park (kBlock);
    }
    for (int b = 0; b < kWakeBlocks; ++b)
        passing();

    REQUIRE (rig.nonFinite == 0);
    REQUIRE (busPeak > 0.1f);
    CHECK_THAT (masterLeak, WithinAbs (0.0, 0.0));
    CHECK_THAT (worst, WithinAbs (0.0, toleranceFor (busPeak)));
}

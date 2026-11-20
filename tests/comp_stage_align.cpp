#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/CompStageAlign.h"

#if DUSKSTUDIO_HAS_DUSK_DSP
  #include "dsp/MultiCompStage.h"
  #include "session/CompParams.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

using Catch::Matchers::WithinAbs;
using duskstudio::CompStageAlign;
using duskstudio::CompStageBlock;
using duskstudio::CompStageClock;
using duskstudio::CompStageProcessor;

namespace
{
constexpr double kSr      = 48000.0;
constexpr int    kLatency = 73;
constexpr int    kBlock   = 256;
constexpr int    kRamp    = 960;   // 20 ms at 48 kHz
constexpr double kTwoPi   = 6.283185307179586;

// A latent compressor that is exactly a gain on its input `latency` samples
// late, so every expected output can be written down.
struct FakeComp final : CompStageProcessor
{
    FakeComp (int latency, float g) : gain (g), histL ((std::size_t) latency, 0.0f), histR (histL) {}

    void process (float* L, float* R, int n) noexcept override
    {
        ++calls;
        for (int i = 0; i < n; ++i)
        {
            const float l = L[i];
            L[i] = gain * histL[at];
            histL[at] = l;
            if (R != nullptr)
            {
                const float r = R[i];
                R[i] = gain * histR[at];
                histR[at] = r;
            }
            at = (at + 1) % histL.size();
        }
    }

    void reset() noexcept override
    {
        ++resets;
        std::fill (histL.begin(), histL.end(), 0.0f);
        std::fill (histR.begin(), histR.end(), 0.0f);
        at = 0;
    }

    float gain;
    std::vector<float> histL, histR;
    std::size_t at = 0;
    int calls = 0, resets = 0;
};

std::vector<float> sine (double hz, float amp, int n, double phase = 0.0)
{
    std::vector<float> v ((std::size_t) n);
    for (int i = 0; i < n; ++i)
        v[(std::size_t) i] = amp * (float) std::sin (kTwoPi * hz * (double) i / kSr + phase);
    return v;
}

// What one block of a run does: whether any compressor of the stage is
// engaged, whether the transport is stopped, and whether this member's is.
struct Step
{
    bool anyEngaged = false;
    bool stopped    = true;
    bool engaged    = false;
};

// Runs L (and R when given) through a member block by block, advancing the
// clock once per block first, as the engine does.
void run (CompStageClock& clock, CompStageAlign& member, CompStageProcessor* comp,
          std::vector<float>& L, std::vector<float>* R, const std::function<Step (int)>& schedule,
          int block = kBlock)
{
    for (std::size_t offset = 0, b = 0; offset < L.size(); offset += (std::size_t) block, ++b)
    {
        const int n = (int) std::min<std::size_t> ((std::size_t) block, L.size() - offset);
        const auto step = schedule ((int) b);
        clock.advance (step.anyEngaged, step.stopped, n);
        member.process (L.data() + offset, R != nullptr ? R->data() + offset : nullptr, n,
                        clock.block(), step.engaged, comp);
    }
}

float maxStep (const std::vector<float>& v)
{
    float worst = 0.0f;
    for (std::size_t i = 1; i < v.size(); ++i)
        worst = std::max (worst, std::abs (v[i] - v[i - 1]));
    return worst;
}

int peakAt (const std::vector<float>& v)
{
    std::size_t best = 0;
    for (std::size_t i = 0; i < v.size(); ++i)
        if (std::abs (v[i]) > std::abs (v[best])) best = i;
    return (int) best;
}

// The largest difference between v and `expected(i)` over [from, to).
float worstAgainst (const std::vector<float>& v, int from, int to, const std::function<float (int)>& expected)
{
    float worst = 0.0f;
    for (int i = from; i < to; ++i)
        worst = std::max (worst, std::abs (v[(std::size_t) i] - expected (i)));
    return worst;
}
} // namespace

TEST_CASE ("CompStageClock: a stage's latency comes with its first compressor and goes only when stopped", "[compressor][comp-stage]")
{
    CompStageClock clock;
    clock.prepare (kSr, kLatency);
    clock.snap (false);
    REQUIRE_FALSE (clock.block().write);
    CHECK (clock.reportedLatencySamples() == 0);

    // Nothing engaged, rolling or not: the stage never holds a latency.
    for (const bool stopped : { true, false })
        for (int b = 0; b < 20; ++b)
        {
            clock.advance (false, stopped, kBlock);
            REQUIRE_FALSE (clock.block().write);
            REQUIRE (clock.reportedLatencySamples() == 0);
        }

    // The block a compressor is engaged in starts filling the delay; the
    // delayed path fades in once a latency of input is stored, over 20 ms.
    clock.advance (true, false, kBlock);
    CHECK (clock.block().write);
    CHECK (clock.block().alignAt == 0);
    CHECK (clock.block().alignDir == 0);
    CHECK (clock.reportedLatencySamples() == kLatency);
    clock.advance (true, false, kBlock);
    CHECK (clock.block().alignDir == 1);
    CHECK (clock.block().rampSamples == kRamp);

    // Switched out while playing, it holds on.
    for (int b = 0; b < 40; ++b)
        clock.advance (false, false, kBlock);
    CHECK (clock.block().settledOn());
    CHECK (clock.reportedLatencySamples() == kLatency);

    // Stopped with none engaged, it fades out and stops storing.
    clock.advance (false, true, kBlock);
    CHECK (clock.block().alignDir == -1);
    CHECK (clock.reportedLatencySamples() == 0);
    int blocks = 0;
    while (clock.block().write && blocks < 100)
    {
        clock.advance (false, true, kBlock);
        ++blocks;
    }
    CHECK_FALSE (clock.block().write);
    CHECK (blocks == (kRamp + kBlock - 1) / kBlock);

    // Engaged again while it fades out, it turns back.
    clock.snap (true);
    clock.advance (false, true, kBlock);
    CHECK (clock.block().alignDir == -1);
    clock.advance (true, true, kBlock);
    CHECK (clock.block().alignDir == 1);

    // A snap lands on the target with no fade, and members see it.
    const auto before = clock.block().snap;
    clock.snap (true);
    CHECK (clock.block().settledOn());
    CHECK (clock.block().snap != before);
    clock.snap (false);
    CHECK_FALSE (clock.block().write);
    CHECK (clock.reportedLatencySamples() == 0);

    // A stage whose plug-in has no latency never turns on.
    CompStageClock none;
    none.prepare (kSr, 0);
    none.snap (true);
    none.advance (true, false, kBlock);
    CHECK_FALSE (none.block().write);
    CHECK (none.reportedLatencySamples() == 0);
}

TEST_CASE ("CompStageClock: the transport rolling again inside the fall puts the stage back on at once", "[compressor][comp-stage]")
{
    // The transport stops at block 10 with nothing engaged, which starts the
    // 20 ms fall, and rolls again at block 11. What a stopped transport plays
    // is silence, then the music from the first rolled sample, here a sine.
    static constexpr int kStop = 10;
    const int total = 30 * kBlock;
    auto in = sine (1000.0, 0.5f, total);
    std::fill (in.begin() + kStop * kBlock, in.begin() + (kStop + 1) * kBlock, 0.0f);

    for (const bool stereo : { false, true })
    {
        CAPTURE (stereo);
        CompStageClock restarted, stayed;
        CompStageAlign restartedMember, stayedMember;
        FakeComp restartedComp (kLatency, 0.5f), stayedComp (kLatency, 0.5f);
        for (auto* c : { &restarted, &stayed })
        {
            c->prepare (kSr, kLatency);
            c->snap (true);
        }
        restartedMember.prepare (kBlock, kLatency);
        stayedMember.prepare (kBlock, kLatency);

        auto L = in, R = in, stayedL = in, stayedR = in;
        std::vector<int> reported;
        for (int b = 0; b * kBlock < total; ++b)
        {
            const bool stopped = b == kStop;
            restarted.advance (false, stopped, kBlock);
            stayed.advance (false, false, kBlock);
            reported.push_back (restarted.reportedLatencySamples());
            CHECK (restarted.latencySamples() == reported.back());
            const auto offset = (std::size_t) (b * kBlock);
            restartedMember.process (L.data() + offset, stereo ? R.data() + offset : nullptr, kBlock,
                                     restarted.block(), false, &restartedComp);
            stayedMember.process (stayedL.data() + offset, stereo ? stayedR.data() + offset : nullptr, kBlock,
                                  stayed.block(), false, &stayedComp);
        }

        // The stopped block falls; the stage reports it off, where it heads.
        CHECK (reported[(std::size_t) kStop - 1] == kLatency);
        CHECK (reported[(std::size_t) kStop] == 0);
        // Rolling, it is on from the first sample: the same latency reported
        // and the same samples played as by a stage that never stopped.
        for (int b = kStop + 1; b * kBlock < total; ++b)
            CHECK (reported[(std::size_t) b] == kLatency);
        CHECK (restarted.block().settledOn());
        const auto from = (std::ptrdiff_t) ((kStop + 1) * kBlock);
        CHECK (std::equal (L.begin() + from, L.end(), stayedL.begin() + from));
        if (stereo) CHECK (std::equal (R.begin() + from, R.end(), stayedR.begin() + from));
        CHECK (worstAgainst (L, (kStop + 1) * kBlock + kLatency, total,
                             [&] (int i) { return in[(std::size_t) (i - kLatency)]; }) <= 0.0f);
        CHECK (restartedComp.calls == 0);
    }
}

TEST_CASE ("CompStageClock: a fall that ends while stopped leaves the stage off when the transport rolls", "[compressor][comp-stage]")
{
    static constexpr int kStop = 10;
    static constexpr int kRoll = kStop + (kRamp + kBlock - 1) / kBlock + 1;
    const int total = (kRoll + 10) * kBlock;
    const auto in = sine (1000.0, 0.5f, total);
    auto L = in;
    CompStageClock clock;
    clock.prepare (kSr, kLatency);
    clock.snap (true);
    CompStageAlign member;
    member.prepare (kBlock, kLatency);
    FakeComp comp (kLatency, 0.5f);
    run (clock, member, &comp, L, nullptr,
         [] (int b) { return Step { false, b >= kStop && b < kRoll, false }; });

    CHECK_FALSE (clock.block().write);
    CHECK (clock.reportedLatencySamples() == 0);
    CHECK (worstAgainst (L, kRoll * kBlock, total, [&] (int i) { return in[(std::size_t) i]; }) <= 0.0f);
    // The fall itself, a fade between the delayed and the undelayed input.
    const float bound = (float) (0.5 * kTwoPi * 1000.0 / kSr) + 2.0f * 0.5f / (float) kRamp + 1.0e-5f;
    CHECK (maxStep (L) <= bound);
}

TEST_CASE ("CompStageAlign: an impulse lands at 0 with the stage off and at the latency with it on", "[compressor][comp-stage]")
{
    for (const bool stereo : { false, true })
    {
        CAPTURE (stereo);
        const int total = 4 * kBlock;
        const int impulseAt = kBlock + 31;
        std::vector<float> in ((std::size_t) total, 0.0f);
        in[(std::size_t) impulseAt] = 0.5f;

        const auto through = [&] (bool on, bool engaged, FakeComp& comp)
        {
            CompStageClock clock;
            clock.prepare (kSr, kLatency);
            clock.snap (on);
            CompStageAlign member;
            member.prepare (kBlock, kLatency);
            std::vector<float> L = in, R = in;
            run (clock, member, &comp, L, stereo ? &R : nullptr,
                 [&] (int) { return Step { on, true, engaged }; });
            if (stereo) CHECK (R == L);
            return L;
        };

        SECTION ("off: the input untouched, nothing runs")
        {
            FakeComp comp (kLatency, 0.5f);
            const auto out = through (false, false, comp);
            CHECK (out == in);
            CHECK (comp.calls == 0);
        }
        SECTION ("on, compressor out: a plain delay")
        {
            FakeComp comp (kLatency, 0.5f);
            const auto out = through (true, false, comp);
            CHECK (peakAt (out) == impulseAt + kLatency);
            CHECK_THAT (out[(std::size_t) (impulseAt + kLatency)], WithinAbs (0.5f, 0.0f));
            CHECK (comp.calls == 0);
        }
        SECTION ("on, compressor in: the compressor's own output")
        {
            FakeComp comp (kLatency, 0.5f);
            const auto out = through (true, true, comp);
            CHECK (peakAt (out) == impulseAt + kLatency);
            CHECK_THAT (out[(std::size_t) (impulseAt + kLatency)], WithinAbs (0.25f, 0.0f));
            CHECK (comp.calls > 0);
        }
    }
}

#if DUSKSTUDIO_HAS_DUSK_DSP
TEST_CASE ("CompStageAlign: a real Multi-Comp 2 lands where the plain delay does", "[compressor][comp-stage][multicomp]")
{
    using duskstudio::CompParamId;
    using duskstudio::CompStage;
    for (const bool stereo : { false, true })
    {
        CAPTURE (stereo);
        duskstudio::MultiCompStage comp (stereo ? CompStage::Bus : CompStage::Strip);
        comp.prepare (kSr, kBlock, stereo ? 2 : 1);
        const int latency = comp.latencySamples();
        REQUIRE (latency > 0);
        duskstudio::CompParams params;
        params.resetToDefaults (stereo ? CompStage::Bus : CompStage::Strip);
        comp.pushParameters (params, true, 0);

        const int total = 8 * kBlock;
        const int impulseAt = 2 * kBlock + 5;
        std::vector<float> in ((std::size_t) total, 0.0f);
        in[(std::size_t) impulseAt] = 0.05f;

        for (const bool engaged : { false, true })
        {
            CAPTURE (engaged);
            CompStageClock clock;
            clock.prepare (kSr, latency);
            clock.snap (true);
            CompStageAlign member;
            member.prepare (kBlock, latency);
            std::vector<float> L = in, R = in;
            run (clock, member, &comp, L, stereo ? &R : nullptr,
                 [&] (int) { return Step { true, true, engaged }; });
            CHECK (peakAt (L) == impulseAt + latency);
            CHECK (member.compressorRunning() == engaged);
        }
    }
}
#endif

TEST_CASE ("CompStageAlign: every transition is click-free and ends where it should", "[compressor][comp-stage]")
{
    // A 100 Hz sine at 0.5 moves at most A * 2 pi f / sr = 0.00654 a sample.
    // A fade over R = 960 samples between two paths whose difference is at
    // most 2A adds at most 2A / R = 0.00104 a sample to that, whichever two
    // paths it joins (the input and its delayed copy, or the delayed dry and
    // the compressor's half-gain output): out = (1 - w) a + w b steps by a
    // convex mix of a's and b's steps plus dw (b - a). A switch with no fade
    // jumps by up to |x[n] - x[n - 73]| = 0.46, or by 0.25 between the dry
    // and the compressor; the bound sits far under both.
    static constexpr float kAmp = 0.5f;
    static constexpr double kHz = 100.0;
    const float bound = (float) (kAmp * kTwoPi * kHz / kSr) + 2.0f * kAmp / (float) kRamp + 1.0e-5f;

    for (const bool stereo : { false, true })
    {
        CAPTURE (stereo);
        // Blocks 0-9 stopped with nothing engaged; 10: another member engages,
        // so this one only takes the latency; 30: this one engages; 60: it
        // is switched out while playing; 90: the transport stops.
        const auto schedule = [] (int b)
        {
            Step s;
            s.stopped    = b < 10 || b >= 90;
            s.anyEngaged = b >= 10 && b < 60;
            s.engaged    = b >= 30 && b < 60;
            return s;
        };
        const int total = 120 * kBlock;
        const auto inL = sine (kHz, kAmp, total);
        const auto inR = sine (kHz, kAmp, total, 1.0);
        auto L = inL, R = inR;
        CompStageClock clock;
        clock.prepare (kSr, kLatency);
        clock.snap (false);
        CompStageAlign member;
        member.prepare (kBlock, kLatency);
        FakeComp comp (kLatency, 0.5f);
        run (clock, member, &comp, L, stereo ? &R : nullptr, schedule);

        CHECK (maxStep (L) <= bound);
        if (stereo) CHECK (maxStep (R) <= bound);

        const auto x = [&] (int i) { return inL[(std::size_t) i]; };
        const auto d = [&] (int i) { return inL[(std::size_t) (i - kLatency)]; };
        // Each steady state, once its fades are done.
        CHECK (worstAgainst (L, 0, 10 * kBlock, x) <= 0.0f);
        CHECK (worstAgainst (L, 20 * kBlock, 30 * kBlock, d) <= 0.0f);
        CHECK (worstAgainst (L, 40 * kBlock, 60 * kBlock, [&] (int i) { return 0.5f * d (i); }) <= 0.0f);
        CHECK (worstAgainst (L, 70 * kBlock, 90 * kBlock, d) <= 0.0f);
        CHECK (worstAgainst (L, 100 * kBlock, total, x) <= 0.0f);
        if (stereo)
            CHECK (worstAgainst (R, 40 * kBlock, 60 * kBlock,
                                 [&] (int i) { return 0.5f * inR[(std::size_t) (i - kLatency)]; }) <= 0.0f);

        // The compressor stopped once its fade was out, and went back to rest.
        CHECK_FALSE (member.compressorRunning());
        CHECK (comp.resets == 1);
    }
}

TEST_CASE ("CompStageAlign: the first compressor engaged waits for the stage's latency, then fades in", "[compressor][comp-stage]")
{
    // The member's own engage is held back until the stage has faded every
    // member onto its delayed path, so until then its output is the input
    // faded into its delayed copy by the stage's weight, with nothing of the
    // compressor in it.
    const int total = 40 * kBlock;
    const auto in = sine (440.0, 0.5f, total);
    auto L = in;
    CompStageClock clock;
    clock.prepare (kSr, kLatency);
    clock.snap (false);
    CompStageAlign member;
    member.prepare (kBlock, kLatency);
    FakeComp comp (kLatency, 0.5f);

    int settledFrom = -1;
    float worstBeforeSettled = 0.0f;
    for (int b = 0; b * kBlock < total; ++b)
    {
        const bool engaged = b >= 4;
        clock.advance (engaged, true, kBlock);
        const auto block = clock.block();
        float* p = L.data() + b * kBlock;
        member.process (p, nullptr, kBlock, block, engaged, &comp);
        if (block.settledOn())
        {
            if (settledFrom < 0) settledFrom = b * kBlock;
            continue;
        }
        for (int i = 0; i < kBlock; ++i)
        {
            const int n = b * kBlock + i;
            const float w = (float) std::clamp (block.alignAt + block.alignDir * (i + 1), 0, block.rampSamples)
                          / (float) block.rampSamples;
            const float x = in[(std::size_t) n];
            const float dry = n >= kLatency ? in[(std::size_t) (n - kLatency)] : 0.0f;
            worstBeforeSettled = std::max (worstBeforeSettled, std::abs (p[i] - (x + w * (dry - x))));
        }
    }
    CAPTURE (settledFrom);
    REQUIRE (settledFrom > 4 * kBlock);
    CHECK (worstBeforeSettled < 1.0e-6f);
    // Then the member's 20 ms fade onto the compressor, and its output after.
    CHECK (worstAgainst (L, settledFrom + kRamp + kBlock, total,
                         [&] (int i) { return 0.5f * in[(std::size_t) (i - kLatency)]; }) <= 0.0f);
}

TEST_CASE ("CompStageAlign: a block over the prepared size runs in parts, and an empty one changes nothing", "[compressor][comp-stage]")
{
    // Every fade, the stage's and the member's, runs across the oversized
    // blocks' inner part boundaries.
    const auto schedule = [] (int b)
    {
        Step s;
        s.stopped = b >= 12;
        s.anyEngaged = b >= 1 && b < 9;
        s.engaged = b >= 2 && b < 9;
        return s;
    };
    const int big = 3 * 64 + 17;
    const int total = 20 * big;
    const auto in = sine (1000.0, 0.3f, total);
    for (const bool stereo : { false, true })
    {
        CAPTURE (stereo);
        auto wholeL = in, wholeR = in, partsL = in, partsR = in;
        CompStageClock wholeClock, partsClock;
        CompStageAlign whole, parts;
        FakeComp wholeComp (kLatency, 0.5f), partsComp (kLatency, 0.5f);
        for (auto* c : { &wholeClock, &partsClock })
        {
            c->prepare (kSr, kLatency);
            c->snap (false);
        }
        whole.prepare (big, kLatency);
        parts.prepare (64, kLatency);
        run (wholeClock, whole, &wholeComp, wholeL, stereo ? &wholeR : nullptr, schedule, big);

        for (int b = 0; b * big < total; ++b)
        {
            const auto s = schedule (b);
            partsClock.advance (s.anyEngaged, s.stopped, big);
            float* l = partsL.data() + b * big;
            float* r = stereo ? partsR.data() + b * big : nullptr;
            parts.process (l, r, big, partsClock.block(), s.engaged, &partsComp);
            // An empty block between two real ones is no block at all.
            parts.process (l, r, 0, partsClock.block(), s.engaged, &partsComp);
        }
        CHECK (partsL == wholeL);
        if (stereo) CHECK (partsR == wholeR);
    }
}

TEST_CASE ("CompStageAlign: a member that missed blocks or was cut replays nothing it held", "[compressor][comp-stage]")
{
    const auto loud = sine (500.0, 0.5f, kBlock);
    const std::vector<float> quiet ((std::size_t) kBlock, 0.0f);
    const auto peak = [] (const std::vector<float>& v)
    {
        float p = 0.0f;
        for (const float x : v) p = std::max (p, std::abs (x));
        return p;
    };
    CompStageClock clock;
    clock.prepare (kSr, kLatency);
    clock.snap (true);
    const auto next = [&] { clock.advance (true, false, kBlock); };

    SECTION ("a plain delay skipped right after audio")
    {
        CompStageAlign skipped, kept;
        for (auto* m : { &skipped, &kept }) m->prepare (kBlock, kLatency);
        auto a = loud, c = loud;
        next();
        skipped.process (a.data(), nullptr, kBlock, clock.block(), false, nullptr);
        kept.process (c.data(), nullptr, kBlock, clock.block(), false, nullptr);
        next();
        c = quiet;
        kept.process (c.data(), nullptr, kBlock, clock.block(), false, nullptr);
        CHECK (peak (c) > 0.1f);   // what the skipped one held
        next();
        a = quiet;
        skipped.process (a.data(), nullptr, kBlock, clock.block(), false, nullptr);
        CHECK (a == quiet);
    }

    SECTION ("a compressor asleep on silence keeps its state")
    {
        // A strip sleeps only once its input has been silent past its tail,
        // so its compressor's delay has drained by then.
        FakeComp comp (kLatency, 1.0f);
        CompStageAlign member;
        member.prepare (kBlock, kLatency);
        auto a = loud;
        next();
        member.process (a.data(), nullptr, kBlock, clock.block(), true, &comp);
        a = quiet;
        next();
        member.process (a.data(), nullptr, kBlock, clock.block(), true, &comp);
        CHECK (peak (a) > 0.1f);
        next();   // asleep
        next();
        a = quiet;
        member.process (a.data(), nullptr, kBlock, clock.block(), true, &comp);
        CHECK (a == quiet);
        CHECK (comp.resets == 0);
        CHECK (member.compressorRunning());
    }

    SECTION ("an output cut, by the member or by the mixer stopping")
    {
        for (const bool byMixer : { false, true })
        {
            CAPTURE (byMixer);
            FakeComp comp (kLatency, 1.0f);
            CompStageAlign member;
            member.prepare (kBlock, kLatency);
            auto a = loud;
            next();
            member.process (a.data(), nullptr, kBlock, clock.block(), true, &comp);
            if (byMixer) clock.markGap();
            else         member.discard();
            next();
            a = quiet;
            member.process (a.data(), nullptr, kBlock, clock.block(), true, &comp);
            CHECK (a == quiet);
            CHECK (comp.resets == 1);
            CHECK (member.compressorRunning());
        }
    }
}

TEST_CASE ("CompStageAlign: a snapped stage starts aligned, compressor in, with no fade", "[compressor][comp-stage]")
{
    std::vector<float> in ((std::size_t) (4 * kBlock), 0.0f);
    in[100] = 0.5f;
    for (const bool engaged : { false, true })
    {
        CAPTURE (engaged);
        CompStageClock clock;
        clock.prepare (kSr, kLatency);
        clock.snap (true);
        CompStageAlign member;
        member.prepare (kBlock, kLatency);
        FakeComp comp (kLatency, 0.5f);
        auto L = in;
        run (clock, member, &comp, L, nullptr, [&] (int) { return Step { true, false, engaged }; });
        CHECK (peakAt (L) == 100 + kLatency);
        CHECK_THAT (L[100 + kLatency], WithinAbs (engaged ? 0.25f : 0.5f, 0.0f));
    }
}

TEST_CASE ("CompStageAlign: a width flip restarts the compressor from rest under the delayed dry", "[compressor][comp-stage]")
{
    // At unity the compressor's output is its delayed input, so a flip that
    // keeps the delayed dry under the restarting compressor moves nothing.
    static constexpr float kAmp = 0.5f;
    const float bound = (float) (kAmp * kTwoPi * 100.0 / kSr) + 1.0e-5f;
    const int total = 40 * kBlock;
    const auto in = sine (100.0, kAmp, total);
    auto L = in, R = in;
    CompStageClock clock;
    clock.prepare (kSr, kLatency);
    clock.snap (true);
    CompStageAlign member;
    member.prepare (kBlock, kLatency);
    FakeComp mono (kLatency, 1.0f), stereo (kLatency, 1.0f);
    for (int b = 0; b * kBlock < total; ++b)
    {
        const bool wide = b >= 20;
        clock.advance (true, false, kBlock);
        member.process (L.data() + b * kBlock, wide ? R.data() + b * kBlock : nullptr, kBlock,
                        clock.block(), true, wide ? &stereo : &mono);
    }
    CHECK (maxStep (L) <= bound);
    CHECK (mono.resets == 1);
    CHECK (stereo.calls > 0);
    CHECK (worstAgainst (L, 30 * kBlock, total, [&] (int i) { return in[(std::size_t) (i - kLatency)]; }) <= 0.0f);
}

#if DUSKSTUDIO_HAS_DUSK_DSP
TEST_CASE ("CompStageAlign: the engage fade onto a compressor that changes nothing keeps a sine's level", "[compressor][comp-stage][multicomp]")
{
    using duskstudio::CompParamId;
    using duskstudio::CompStage;
    // The bus compressor with its threshold at the top of its range never
    // reduces a -20 dBFS sine, so what it adds is its own colour on a signal
    // a latency late. Faded in and out against a dry delayed by the same
    // latency, the level stays within that colour: 0.02 dB measured at both
    // frequencies, held to 0.1 dB. A fade against an undelayed dry would comb:
    // halfway through it, a tone summed with itself 73 samples late loses
    // |cos (pi f 73 / 48000)|, 24 dB at 1 kHz and 5 dB at 9 kHz.
    for (const double hz : { 1000.0, 9000.0 })
    {
        CAPTURE (hz);
        duskstudio::MultiCompStage comp (CompStage::Bus);
        comp.prepare (kSr, kBlock, 2);
        const int latency = comp.latencySamples();
        duskstudio::CompParams params;
        params.resetToDefaults (CompStage::Bus);
        params.set (CompParamId::BusThreshold, 15.0f);
        comp.pushParameters (params, true, 0);

        CompStageClock clock;
        clock.prepare (kSr, latency);
        clock.snap (true);
        CompStageAlign member;
        member.prepare (kBlock, latency);

        const int total = 60 * kBlock;
        const float amp = 0.1f;
        const auto in = sine (hz, amp, total);
        auto L = in, R = in;
        // Engaged at block 20, switched out again at block 40.
        run (clock, member, &comp, L, &R,
             [] (int b) { return Step { true, false, b >= 20 && b < 40 }; });

        // Level over every window of one 1 kHz period (48 samples) from the
        // first delayed block on, against the sine's own RMS.
        const double steady = amp / std::sqrt (2.0);
        double worstDb = 0.0;
        static constexpr int window = 48;
        for (int from = 2 * kBlock; from + window <= total; from += window / 2)
        {
            double sum = 0.0;
            for (int i = from; i < from + window; ++i)
                sum += (double) L[(std::size_t) i] * L[(std::size_t) i];
            const double db = 20.0 * std::log10 (std::sqrt (sum / window) / steady);
            worstDb = std::max (worstDb, std::abs (db));
        }
        CAPTURE (worstDb);
        CHECK (worstDb < 0.1);
    }
}
#endif

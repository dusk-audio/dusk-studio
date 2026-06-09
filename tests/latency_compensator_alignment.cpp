#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/LatencyCompensator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using Catch::Matchers::WithinAbs;
using duskstudio::LatencyCompensator;

namespace
{
constexpr double kSr    = 48000.0;
constexpr int    kBlock = 256;

struct Capture
{
    std::vector<float> L, R;

    void append (const float* l, const float* r, int n)
    {
        L.insert (L.end(), l, l + n);
        R.insert (R.end(), r, r + n);
    }
};

// Run `numBlocks` blocks through the compensator. The dry path carries an
// impulse at absolute sample `dryImpulseAt`; each active lane carries an
// impulse at `laneImpulseAt[lane]` (negative = lane silent), modelling the
// wet signal a latent plugin chain would emit. Returns the summed
// dry + returns stream.
Capture runStream (LatencyCompensator& comp,
                   int numBlocks,
                   int dryImpulseAt,
                   const std::vector<int>& laneImpulseAt)
{
    Capture out;
    std::vector<float> dryL ((size_t) kBlock), dryR ((size_t) kBlock);
    std::vector<float> laneL ((size_t) kBlock), laneR ((size_t) kBlock);

    for (int b = 0; b < numBlocks; ++b)
    {
        const int blockStart = b * kBlock;

        std::fill (dryL.begin(), dryL.end(), 0.0f);
        std::fill (dryR.begin(), dryR.end(), 0.0f);
        if (dryImpulseAt >= blockStart && dryImpulseAt < blockStart + kBlock)
        {
            dryL[(size_t) (dryImpulseAt - blockStart)] = 1.0f;
            dryR[(size_t) (dryImpulseAt - blockStart)] = 1.0f;
        }

        comp.processDryPath (dryL.data(), dryR.data(), kBlock);

        for (size_t lane = 0; lane < laneImpulseAt.size(); ++lane)
        {
            const int at = laneImpulseAt[lane];
            if (at < 0) continue;

            std::fill (laneL.begin(), laneL.end(), 0.0f);
            std::fill (laneR.begin(), laneR.end(), 0.0f);
            if (at >= blockStart && at < blockStart + kBlock)
            {
                laneL[(size_t) (at - blockStart)] = 1.0f;
                laneR[(size_t) (at - blockStart)] = 1.0f;
            }

            comp.processAuxReturn ((int) lane, laneL.data(), laneR.data(), kBlock);

            for (int i = 0; i < kBlock; ++i)
            {
                dryL[(size_t) i] += laneL[(size_t) i];
                dryR[(size_t) i] += laneR[(size_t) i];
            }
        }

        out.append (dryL.data(), dryR.data(), kBlock);
    }
    return out;
}

int countAbove (const std::vector<float>& v, float thresh)
{
    int n = 0;
    for (float x : v)
        if (std::abs (x) > thresh) ++n;
    return n;
}
} // namespace

TEST_CASE ("LatencyCompensator aligns a latent aux return with the dry path")
{
    LatencyCompensator comp;
    comp.prepare (kSr, kBlock);

    constexpr int kLaneLatency = 333; // deliberately not block-aligned

    comp.setAuxLatency (0, kLaneLatency);

    // Dry impulse fires at t=0; the lane's wet impulse arrives kLaneLatency
    // late (the plugin chain's delay). After compensation both land at
    // maxAux, summing to a single 2.0 peak.
    const auto out = runStream (comp, 4, 0, { kLaneLatency, -1, -1, -1 });

    REQUIRE (comp.isActive());
    REQUIRE (comp.getMaxAuxLatency() == kLaneLatency);

    REQUIRE_THAT (out.L[(size_t) kLaneLatency], WithinAbs (2.0, 1e-6));
    REQUIRE_THAT (out.R[(size_t) kLaneLatency], WithinAbs (2.0, 1e-6));
    REQUIRE (countAbove (out.L, 1e-6f) == 1);
    REQUIRE (countAbove (out.R, 1e-6f) == 1);
}

TEST_CASE ("LatencyCompensator with zero latency is a bit-exact bypass")
{
    LatencyCompensator comp;
    comp.prepare (kSr, kBlock);

    REQUIRE_FALSE (comp.isActive());
    REQUIRE (comp.getMaxAuxLatency() == 0);

    std::vector<float> srcL ((size_t) kBlock), srcR ((size_t) kBlock);
    for (int i = 0; i < kBlock; ++i)
    {
        srcL[(size_t) i] = std::sin (0.01f * (float) i) * 0.7f;
        srcR[(size_t) i] = std::cos (0.013f * (float) i) * 0.4f;
    }

    SECTION ("dry path untouched")
    {
        auto L = srcL, R = srcR;
        comp.processDryPath (L.data(), R.data(), kBlock);
        REQUIRE (std::memcmp (L.data(), srcL.data(), sizeof (float) * (size_t) kBlock) == 0);
        REQUIRE (std::memcmp (R.data(), srcR.data(), sizeof (float) * (size_t) kBlock) == 0);
    }

    SECTION ("aux returns untouched")
    {
        comp.processDryPath (nullptr, nullptr, 0);
        for (int lane = 0; lane < LatencyCompensator::kNumLanes; ++lane)
        {
            auto L = srcL, R = srcR;
            comp.processAuxReturn (lane, L.data(), R.data(), kBlock);
            REQUIRE (std::memcmp (L.data(), srcL.data(), sizeof (float) * (size_t) kBlock) == 0);
            REQUIRE (std::memcmp (R.data(), srcR.data(), sizeof (float) * (size_t) kBlock) == 0);
        }
    }
}

TEST_CASE ("LatencyCompensator aligns multiple lanes with unequal latencies")
{
    LatencyCompensator comp;
    comp.prepare (kSr, kBlock);

    constexpr int kLatA = 120;
    constexpr int kLatB = 450; // > one block

    comp.setAuxLatency (0, kLatA);
    comp.setAuxLatency (1, kLatB);

    const auto out = runStream (comp, 4, 0, { kLatA, kLatB, -1, -1 });

    REQUIRE (comp.getMaxAuxLatency() == kLatB);

    // Dry + lane A + lane B all coincide at max(kLatA, kLatB) -> 3.0.
    REQUIRE_THAT (out.L[(size_t) kLatB], WithinAbs (3.0, 1e-6));
    REQUIRE_THAT (out.R[(size_t) kLatB], WithinAbs (3.0, 1e-6));
    REQUIRE (countAbove (out.L, 1e-6f) == 1);
    REQUIRE (countAbove (out.R, 1e-6f) == 1);
}

TEST_CASE ("LatencyCompensator max-latency lane passes with zero added delay")
{
    LatencyCompensator comp;
    comp.prepare (kSr, kBlock);

    comp.setAuxLatency (2, 200);

    // Prime the per-block target application.
    std::vector<float> z ((size_t) kBlock, 0.0f);
    comp.processDryPath (z.data(), z.data(), kBlock);

    // Lane 2 IS the max lane: its return needs no extra delay, so the
    // buffer must come through bit-unchanged.
    std::vector<float> srcL ((size_t) kBlock), srcR ((size_t) kBlock);
    for (int i = 0; i < kBlock; ++i)
    {
        srcL[(size_t) i] = 0.25f * (float) ((i % 7) - 3);
        srcR[(size_t) i] = 0.1f  * (float) ((i % 5) - 2);
    }
    auto L = srcL, R = srcR;
    comp.processAuxReturn (2, L.data(), R.data(), kBlock);
    REQUIRE (std::memcmp (L.data(), srcL.data(), sizeof (float) * (size_t) kBlock) == 0);
    REQUIRE (std::memcmp (R.data(), srcR.data(), sizeof (float) * (size_t) kBlock) == 0);
}

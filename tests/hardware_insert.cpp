#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/HardwareInsertSlot.h"
#include "session/Session.h"

#include <cmath>
#include <memory>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSr        = 48000.0;
constexpr int    kBlock     = 512;
constexpr double kSmoothMs  = 20.0;

// SmoothedValue ramps over 20 ms (≈960 samples @ 48 k). At kBlock = 512
// per call, one block lands ~half-way to the target. Process N blocks
// before measuring so the gains and the dry/wet mix have fully settled.
// 6 blocks × 512 = 3072 samples = ~64 ms, well past 3 ramp time-constants.
constexpr int kSettleBlocks = 6;

// Helper: publish a fresh routing snapshot into a HardwareInsertParams.
void setRouting (duskstudio::HardwareInsertParams& p,
                  int outL, int outR, int inL, int inR,
                  int latencySamples, int format)
{
    p.routing.publish (std::make_unique<duskstudio::HardwareInsertRouting> (
        duskstudio::HardwareInsertRouting {
            outL, outR, inL, inR, latencySamples, format
        }));
}

// Helper: pretend the audio device has `numCh` input channels each
// filled with `fill` and `numCh` output channels zeroed.
struct DeviceBuffers
{
    std::vector<std::vector<float>> inStore;
    std::vector<std::vector<float>> outStore;
    std::vector<const float*>       inPtrs;
    std::vector<float*>             outPtrs;

    DeviceBuffers (int numCh, int numSamples, float inFill = 0.0f)
    {
        inStore .assign ((size_t) numCh, std::vector<float> ((size_t) numSamples, inFill));
        outStore.assign ((size_t) numCh, std::vector<float> ((size_t) numSamples, 0.0f));
        inPtrs .reserve ((size_t) numCh);
        outPtrs.reserve ((size_t) numCh);
        for (auto& row : inStore)  inPtrs .push_back (row.data());
        for (auto& row : outStore) outPtrs.push_back (row.data());
    }

    void zeroOutputs()
    {
        for (auto& row : outStore) std::fill (row.begin(), row.end(), 0.0f);
    }

    void setInputConst (int ch, float v)
    {
        std::fill (inStore[(size_t) ch].begin(), inStore[(size_t) ch].end(), v);
    }
};

// Helper: build + bind + prepare a slot.
std::unique_ptr<duskstudio::HardwareInsertSlot> makeSlot (duskstudio::HardwareInsertParams& params)
{
    auto slot = std::make_unique<duskstudio::HardwareInsertSlot>();
    slot->prepare (kSr, kBlock);
    slot->bind (params);
    return slot;
}
} // namespace

TEST_CASE ("HardwareInsertSlot: dryWet=0 returns dry signal unchanged at zero latency",
            "[HardwareInsertSlot]")
{
    duskstudio::HardwareInsertParams params;
    setRouting (params, -1, -1, -1, -1, 0, 0);   // routing irrelevant, dry-only
    params.dryWet.store (0.0f);                  // 100 % dry
    auto slot = makeSlot (params);

    DeviceBuffers dev (2, kBlock);

    // Run a sine through the strip. Output buffer should equal input.
    std::vector<float> L (kBlock), R (kBlock);
    auto fillSignal = [&]
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const auto s = (float) std::sin (2.0 * 3.14159265358979323846
                                                * 440.0 * (double) i / kSr);
            L[(size_t) i] = 0.5f * s;
            R[(size_t) i] = 0.3f * s;
        }
    };

    // Warm-up so dryWet smoother lands on 0.
    for (int b = 0; b < kSettleBlocks; ++b)
    {
        fillSignal();
        slot->processStereoBlock (L.data(), R.data(), kBlock,
                                    dev.inPtrs.data(), 2,
                                    dev.outPtrs.data(), 2);
    }

    // Measurement block.
    fillSignal();
    std::vector<float> inputL = L, inputR = R;
    dev.zeroOutputs();
    slot->processStereoBlock (L.data(), R.data(), kBlock,
                                dev.inPtrs.data(), 2,
                                dev.outPtrs.data(), 2);

    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE_THAT (L[(size_t) i], WithinAbs (inputL[(size_t) i], 1.0e-6f));
        REQUIRE_THAT (R[(size_t) i], WithinAbs (inputR[(size_t) i], 1.0e-6f));
    }
}

TEST_CASE ("HardwareInsertSlot: dry-path delay aligns dry with hardware latency",
            "[HardwareInsertSlot]")
{
    // At dryWet=0 (dry-only), the strip output equals the dry path
    // delayed by latencySamples. A unit impulse at i=0 must surface at
    // i=latencySamples after processing.
    constexpr int kLatency = 64;

    duskstudio::HardwareInsertParams params;
    setRouting (params, 0, 1, 0, 1, kLatency, 0);
    params.dryWet.store (0.0f);                  // 100 % dry, latency aligned
    auto slot = makeSlot (params);

    DeviceBuffers dev (2, kBlock);

    // Warm-up.
    std::vector<float> warm (kBlock, 0.0f);
    for (int b = 0; b < kSettleBlocks; ++b)
    {
        std::fill (warm.begin(), warm.end(), 0.0f);
        std::vector<float> warmR (kBlock, 0.0f);
        slot->processStereoBlock (warm.data(), warmR.data(), kBlock,
                                    dev.inPtrs.data(), 2,
                                    dev.outPtrs.data(), 2);
    }

    // Impulse at i=0 in both channels; expect it back at i=kLatency.
    std::vector<float> L (kBlock, 0.0f), R (kBlock, 0.0f);
    L[0] = 1.0f;
    R[0] = 1.0f;
    dev.zeroOutputs();
    slot->processStereoBlock (L.data(), R.data(), kBlock,
                                dev.inPtrs.data(), 2,
                                dev.outPtrs.data(), 2);

    REQUIRE_THAT (L[(size_t) kLatency], WithinAbs (1.0f, 1.0e-5f));
    REQUIRE_THAT (R[(size_t) kLatency], WithinAbs (1.0f, 1.0e-5f));
    // No spurious peaks elsewhere.
    REQUIRE_THAT (L[0],               WithinAbs (0.0f, 1.0e-5f));
    REQUIRE_THAT (L[(size_t) kLatency - 1], WithinAbs (0.0f, 1.0e-5f));
    REQUIRE_THAT (L[(size_t) kLatency + 1], WithinAbs (0.0f, 1.0e-5f));
}

TEST_CASE ("HardwareInsertSlot: Mid/Side encode + decode round-trip is unity gain",
            "[HardwareInsertSlot]")
{
    // The DSP encodes the SEND and decodes the RETURN in one process
    // call. We can validate encode + decode separately:
    //
    //   ENCODE: input (L, R), format=M/S, dryWet=1. Output of the SEND
    //   side is (M, S) = (0.5*(L+R), 0.5*(L-R)) in deviceOutputs.
    //
    //   DECODE: deviceInputs pre-loaded with (M, S). Strip output =
    //   (M+S, M-S) = original (L, R) at unity gain.

    duskstudio::HardwareInsertParams params;
    setRouting (params, 0, 1, 0, 1, 0, /*format = M/S*/ 1);
    params.dryWet.store (1.0f);                  // 100 % wet
    auto slot = makeSlot (params);

    DeviceBuffers dev (2, kBlock);

    // Build the encoded (M, S) on the inputs from a known (L, R).
    constexpr float kL = 0.5f;
    constexpr float kR = -0.3f;
    const float kM = 0.5f * (kL + kR);
    const float kS = 0.5f * (kL - kR);
    dev.setInputConst (0, kM);
    dev.setInputConst (1, kS);

    std::vector<float> L (kBlock, kL), R (kBlock, kR);

    // Warm-up (dryWet smoother + delay line settle).
    for (int b = 0; b < kSettleBlocks; ++b)
    {
        std::fill (L.begin(), L.end(), kL);
        std::fill (R.begin(), R.end(), kR);
        dev.zeroOutputs();
        slot->processStereoBlock (L.data(), R.data(), kBlock,
                                    dev.inPtrs.data(), 2,
                                    dev.outPtrs.data(), 2);
    }

    // Measurement.
    std::fill (L.begin(), L.end(), kL);
    std::fill (R.begin(), R.end(), kR);
    dev.zeroOutputs();
    slot->processStereoBlock (L.data(), R.data(), kBlock,
                                dev.inPtrs.data(), 2,
                                dev.outPtrs.data(), 2);

    // DECODE check (strip output = original L/R, unity gain).
    REQUIRE_THAT (L[(size_t) kBlock - 1], WithinAbs (kL, 1.0e-5f));
    REQUIRE_THAT (R[(size_t) kBlock - 1], WithinAbs (kR, 1.0e-5f));

    // ENCODE check (deviceOutputs hold the 0.5-scaled M/S of the input).
    REQUIRE_THAT (dev.outStore[0][(size_t) kBlock - 1], WithinAbs (kM, 1.0e-5f));
    REQUIRE_THAT (dev.outStore[1][(size_t) kBlock - 1], WithinAbs (kS, 1.0e-5f));
}

TEST_CASE ("HardwareInsertSlot: outputGain and inputGain compose independently",
            "[HardwareInsertSlot]")
{
    duskstudio::HardwareInsertParams params;
    setRouting (params, 0, 1, 0, 1, 0, 0);   // Stereo format
    params.dryWet.store (1.0f);              // 100 % wet so the return drives the output
    params.outputGainDb.store (-6.0f);       // SEND trim -6 dB ≈ 0.5012
    params.inputGainDb .store (-6.0f);       // RETURN trim -6 dB ≈ 0.5012
    auto slot = makeSlot (params);

    DeviceBuffers dev (2, kBlock);
    constexpr float kRet = 1.0f;
    dev.setInputConst (0, kRet);
    dev.setInputConst (1, kRet);

    constexpr float kSig = 0.8f;
    std::vector<float> L (kBlock, kSig), R (kBlock, kSig);

    // Warm-up so the gain smoothers settle on the -6 dB targets.
    for (int b = 0; b < kSettleBlocks; ++b)
    {
        std::fill (L.begin(), L.end(), kSig);
        std::fill (R.begin(), R.end(), kSig);
        dev.zeroOutputs();
        slot->processStereoBlock (L.data(), R.data(), kBlock,
                                    dev.inPtrs.data(), 2,
                                    dev.outPtrs.data(), 2);
    }

    // Measurement.
    std::fill (L.begin(), L.end(), kSig);
    std::fill (R.begin(), R.end(), kSig);
    dev.zeroOutputs();
    slot->processStereoBlock (L.data(), R.data(), kBlock,
                                dev.inPtrs.data(), 2,
                                dev.outPtrs.data(), 2);

    constexpr float kHalf = 0.5011872336f;   // -6 dB in linear gain

    // Strip output = inputGain × deviceInput. Read late in the block so
    // both smoothers have fully settled.
    REQUIRE_THAT (L[(size_t) kBlock - 1], WithinAbs (kHalf * kRet, 1.0e-4f));
    REQUIRE_THAT (R[(size_t) kBlock - 1], WithinAbs (kHalf * kRet, 1.0e-4f));

    // SEND side = outputGain × dry signal.
    REQUIRE_THAT (dev.outStore[0][(size_t) kBlock - 1],
                   WithinAbs (kHalf * kSig, 1.0e-4f));
    REQUIRE_THAT (dev.outStore[1][(size_t) kBlock - 1],
                   WithinAbs (kHalf * kSig, 1.0e-4f));
}

TEST_CASE ("HardwareInsertSlot: invalid routing falls through to dry-only path",
            "[HardwareInsertSlot]")
{
    // Stale routing (channels out of range for the test 2-in/2-out
    // device) must NOT crash and must produce sensible audio: dry
    // signal back at the strip output, deviceOutputs untouched, return
    // path silent.
    duskstudio::HardwareInsertParams params;
    setRouting (params, 7, 8, 7, 8, 0, 0);
    params.dryWet.store (0.0f);   // dry-only is the safe fallback
    auto slot = makeSlot (params);

    DeviceBuffers dev (2, kBlock, /*inFill*/ 0.9f);

    std::vector<float> L (kBlock, 0.25f), R (kBlock, -0.25f);
    auto inputL = L, inputR = R;

    for (int b = 0; b < kSettleBlocks; ++b)
    {
        std::fill (L.begin(), L.end(), 0.25f);
        std::fill (R.begin(), R.end(), -0.25f);
        dev.zeroOutputs();
        slot->processStereoBlock (L.data(), R.data(), kBlock,
                                    dev.inPtrs.data(), 2,
                                    dev.outPtrs.data(), 2);
    }

    std::fill (L.begin(), L.end(), 0.25f);
    std::fill (R.begin(), R.end(), -0.25f);
    dev.zeroOutputs();
    slot->processStereoBlock (L.data(), R.data(), kBlock,
                                dev.inPtrs.data(), 2,
                                dev.outPtrs.data(), 2);

    REQUIRE_THAT (L[(size_t) kBlock - 1], WithinAbs (inputL[0], 1.0e-5f));
    REQUIRE_THAT (R[(size_t) kBlock - 1], WithinAbs (inputR[0], 1.0e-5f));
    // SEND outputs untouched - the invalid channel indices kept us out.
    REQUIRE_THAT (dev.outStore[0][0], WithinAbs (0.0f, 1.0e-6f));
    REQUIRE_THAT (dev.outStore[1][0], WithinAbs (0.0f, 1.0e-6f));
}

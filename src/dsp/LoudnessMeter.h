#pragma once

#include "../foundation/StereoOversampler.h"

#include <dsp/DuskFilters.hpp>

#include <atomic>

namespace duskstudio
{
// Stereo loudness meter conforming to ITU BS.1770-4 / EBU R128. Computes:
//   - Momentary LUFS  - sliding 400 ms window
//   - Short-term LUFS - sliding 3-second window
//   - Integrated LUFS - running mean across the program with the standard
//                        absolute (−70 LUFS) and relative (−10 LU) gates
//   - True peak (dBTP) - max |x| in the 4×-upsampled domain (ITU BS.1770
//                         Annex 2). Catches inter-sample peaks that the raw
//                         sample stream hides; required for streaming-platform
//                         compliance (Spotify / Apple Music both reject
//                         masters > −1 dBTP).
//
// Algorithm:
//   1. Each input sample is K-weighted (high-shelf @ ~1500 Hz, then high-pass
//      @ ~38 Hz; coefficients regenerated for the prepared sample rate).
//   2. Squared K-weighted samples accumulate into 100 ms "blocks" - the
//      atomic unit of BS.1770 measurement.
//   3. Each completed block is pushed into a ring buffer; sliding-window
//      means over the last 4 blocks (M) and 30 blocks (S) give those
//      readings. The integrated reading averages the gated 400 ms windows
//      (the M window, one per 100 ms step) since the last reset.
//
// Threading:
//   - prepare() / reset() - message thread with audio quiesced (prepare path).
//   - requestReset()      - any thread while audio runs; the state reset is
//                            deferred to the next process() block.
//   - process()           - audio thread, no allocation.
//   - getXxxLufs() / getSamplePeakDb() - atomic loads, any thread.
class LoudnessMeter
{
public:
    LoudnessMeter();

    // Message thread, audio quiesced.
    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    // Safe with audio running: zeroes the published readings immediately and
    // defers the state reset to the next process() block, so the histogram and
    // ring wipes never race the audio thread.
    void requestReset() noexcept
    {
        // Publish the request first. process() checks again after publishing a
        // block, so an in-flight pass reapplies a mid-block request before it
        // returns.
        resetRequested.store (true, std::memory_order_release);
        momentaryLufs.store (-100.0f, std::memory_order_relaxed);
        shortTermLufs.store (-100.0f, std::memory_order_relaxed);
        integratedLufs.store (-100.0f, std::memory_order_relaxed);
        truePeakDb.store    (-100.0f, std::memory_order_relaxed);
    }

    // Audio thread. L and R must each be at least `numSamples` floats.
    // `numSamples` may exceed an internal block boundary; we wrap through
    // multiple blocks within one call.
    void process (const float* L, const float* R, int numSamples) noexcept;

    // Atomic accessors.
    float getMomentaryLufs() const noexcept   { return momentaryLufs.load (std::memory_order_relaxed); }
    float getShortTermLufs() const noexcept   { return shortTermLufs.load (std::memory_order_relaxed); }
    float getIntegratedLufs() const noexcept  { return integratedLufs.load (std::memory_order_relaxed); }
    float getTruePeakDb() const noexcept      { return truePeakDb.load (std::memory_order_relaxed); }

private:
    void finishBlock();
    void publishIntegrated() noexcept;
    static int gateBinFor (double meanSquared) noexcept;

    static constexpr int kMomentaryBlocks  = 4;    // 4 × 100 ms = 400 ms
    static constexpr int kShortTermBlocks  = 30;   // 30 × 100 ms = 3 s

    double sr = 0.0;

    // K-weighting - two cascaded biquads per channel (one per stage).
    duskaudio::Biquad kStage1L, kStage1R;
    duskaudio::Biquad kStage2L, kStage2R;

    // Per-block accumulators.
    int    blockSamplesRemaining = 0;
    int    blockSize             = 0;        // samples per 100 ms block
    double blockSumSquared       = 0.0;       // sum of (L^2 + R^2) of K-weighted samples

    // Integrated gating. The absolute (-70 LUFS) gate is a fixed threshold, so
    // the blocks passing it accumulate into a running sum. The relative gate
    // moves with the program mean, so its block set cannot: publishing needs
    // the summed energy of every block above a threshold only known at publish
    // time. A histogram over block loudness keeps that lookup at kGateBins, so
    // the callback's cost per block is fixed rather than growing with program
    // length - holding one entry per block instead means scanning 36000 of
    // them, twice, an hour into a session.
    //
    // Bins carry the true summed energy of the blocks in them, so every bin
    // except the gate's own is exact; blocks sharing the gate's bin are decided
    // as a group. Measured against the exact two-pass form, the worst case over
    // uniform, two-level, lognormal, long-fade and gate-straddling programs is
    // 0.03 LU, inside EBU Tech 3341's +/-0.1 LU meter tolerance.
    static constexpr int    kGateBinsPerLu = 10;                      // 0.1 LU
    static constexpr double kGateFloorLufs = -70.0;                   // absolute gate
    static constexpr int    kGateBins      = 94 * kGateBinsPerLu + 1; // to +24 LUFS

    double gateBinSum       [kGateBins] = {};
    int    gateBinCount     [kGateBins] = {};
    double absoluteGateSum   = 0.0;
    int    absoluteGateCount = 0;

    double  momentaryRingMS [kMomentaryBlocks] = {};
    double  shortTermRingMS [kShortTermBlocks] = {};
    int     ringWritePos = 0;

    // Live true peak (linear, not dB) - measured in the 4× oversampled
    // domain so inter-sample peaks are caught.
    float currentTruePeak = 0.0f;

    // 4× oversampler for true-peak detection. Used purely for measurement
    // - we scan the upsampled samples for the peak and discard them (never
    // downsample). Sized at prepare(); audio-thread confined.
    dusk::audio::StereoOversampler oversampler;
    int preparedMaxBlockSize = 0;

    std::atomic<float> momentaryLufs   { -100.0f };
    std::atomic<float> shortTermLufs   { -100.0f };
    std::atomic<float> integratedLufs  { -100.0f };
    std::atomic<float> truePeakDb      { -100.0f };
    std::atomic<bool>  resetRequested   { false };
};
} // namespace duskstudio

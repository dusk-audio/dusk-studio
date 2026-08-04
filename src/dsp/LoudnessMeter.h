#pragma once

#include "../foundation/StereoOversampler.h"

#include <dsp/DuskFilters.hpp>

#include <atomic>
#include <vector>

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
    // defers the state reset to the next process() block, so the window and
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
    void startRelativeScan() noexcept;
    void advanceRelativeScan (int numSamples) noexcept;

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
    // the windows passing it accumulate into running sums, exactly and in O(1).
    // The relative gate moves with the program mean, so its window set has to
    // be recomputed against a threshold only known at publish time.
    //
    // That second pass reads every retained window rather than a bucketed
    // summary of them. Bucketing bounds the work but cannot be made exact: the
    // gate can fall inside a bucket, and the windows sharing it then have to be
    // included or excluded as a group. On a program whose quiet material sits
    // at the gate - a single loud window against ten 0.01 LU below it - that is
    // 10 LU of error, and no choice of bucket edge or width fixes it, since
    // equal-valued windows always share a bucket.
    //
    // Scanning is what costs, so the scan is spread across callbacks in
    // proportion to elapsed audio (the budgeting the hardware-insert ping
    // uses), completing inside one 100 ms gating period. One unamortised pass
    // measures 27 us, which is 16 % of a 16-sample callback at 96 kHz.
    static constexpr int kMaxWindows = 36000;   // 1 hour at one per 100 ms

    // Sized once in prepare() so reset() - which runs on the audio thread for a
    // deferred requestReset - never allocates. Saturating at kMaxWindows freezes
    // the integrated reading rather than dropping the oldest material, which
    // would silently redefine "entire program".
    std::vector<double> windowMS;
    int    windowCount       = 0;
    double absoluteGateSum   = 0.0;
    int    absoluteGateCount = 0;

    // In-flight relative-gate scan over windowMS[0, scanLimit). scanLimit == 0
    // means idle; finishBlock starts the next pass once one completes.
    double scanGateMS = 0.0;
    double scanSum    = 0.0;
    double scanCredit = 0.0;
    int    scanCount  = 0;
    int    scanPos    = 0;
    int    scanLimit  = 0;

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

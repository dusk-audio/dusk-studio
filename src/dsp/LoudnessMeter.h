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
//      readings. The integrated reading averages all gated blocks since
//      the last reset.
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
    // defers the state reset to the next process() block, so the vector clear
    // and ring wipes never race the audio thread.
    void requestReset() noexcept
    {
        momentaryLufs.store (-100.0f, std::memory_order_relaxed);
        shortTermLufs.store (-100.0f, std::memory_order_relaxed);
        integratedLufs.store (-100.0f, std::memory_order_relaxed);
        truePeakDb.store    (-100.0f, std::memory_order_relaxed);
        integratedCapped.store (false, std::memory_order_relaxed);
        resetRequested.store (true, std::memory_order_release);
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
    // True once the block-history ring has hit kMaxHistoryBlocks (1 hour
    // at 100 ms per block). After that the integrated reading stops
    // absorbing new blocks. UI surfaces this so the user knows the
    // integrated value is frozen rather than silently inaccurate.
    bool isIntegratedCapped() const noexcept  { return integratedCapped.load (std::memory_order_relaxed); }

private:
    void finishBlock();

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

    // Block history. Stored as MS-per-block; LUFS is computed on demand
    // from sliding-window means. Capped at kMaxHistoryBlocks (1 hour at
    // 100 ms per block) so the audio-thread push_back in finishBlock never
    // reallocates. Sessions longer than an hour silently saturate - the
    // integrated reading stops absorbing new blocks past the cap.
    static constexpr int kMaxHistoryBlocks = 36000;
    std::vector<double> blockHistory;
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
    // Set once when blockHistory hits kMaxHistoryBlocks; the integrated
    // reading stops absorbing new blocks past that point. Audio thread
    // sets via relaxed store on the first overflow, UI polls via the
    // public accessor.
    std::atomic<bool>  integratedCapped { false };
    std::atomic<bool>  resetRequested   { false };
};
} // namespace duskstudio

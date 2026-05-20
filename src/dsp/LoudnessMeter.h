#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <vector>

namespace duskstudio
{
// Stereo loudness meter conforming to ITU BS.1770-4 / EBU R128. Computes:
//   • Momentary LUFS  - sliding 400 ms window
//   • Short-term LUFS - sliding 3-second window
//   • Integrated LUFS - running mean across the program with the standard
//                        absolute (−70 LUFS) and relative (−10 LU) gates
//   • True peak (dBTP) - max |x| in the 4×-upsampled domain (ITU BS.1770
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
//   • prepare() / reset() - message thread.
//   • process()           - audio thread, no allocation.
//   • getXxxLufs() / getSamplePeakDb() - atomic loads, any thread.
class LoudnessMeter
{
public:
    LoudnessMeter();

    // Message thread.
    void prepare (double sampleRate, int maxBlockSize);
    void reset();

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

    static constexpr int kMomentaryBlocks  = 4;    // 4 × 100 ms = 400 ms
    static constexpr int kShortTermBlocks  = 30;   // 30 × 100 ms = 3 s

    double sr = 0.0;

    // K-weighting - two cascaded biquads per channel (one per stage).
    juce::dsp::IIR::Filter<float> kStage1L, kStage1R;
    juce::dsp::IIR::Filter<float> kStage2L, kStage2R;

    // Per-block accumulators.
    int    blockSamplesRemaining = 0;
    int    blockSize             = 0;        // samples per 100 ms block
    double blockSumSquared       = 0.0;       // sum of (L^2 + R^2) of K-weighted samples

    // Block history. Stored as MS-per-block; LUFS is computed on demand
    // from sliding-window means.
    std::vector<double> blockHistory;        // unbounded, used for integrated
    double  momentaryRingMS [kMomentaryBlocks] = {};
    double  shortTermRingMS [kShortTermBlocks] = {};
    int     ringWritePos = 0;

    // Live true peak (linear, not dB) - measured in the 4× oversampled
    // domain so inter-sample peaks are caught.
    float currentTruePeak = 0.0f;

    // 4× oversampler for true-peak detection. Used purely for measurement
    // - we don't keep the upsampled audio. Sized at prepare(); thread
    // confined to the audio thread for processing.
    juce::dsp::Oversampling<float> oversampler;
    juce::AudioBuffer<float>       oversampleInput;  // 2 ch × maxBlockSize, pre-allocated
    int preparedMaxBlockSize = 0;

    std::atomic<float> momentaryLufs   { -100.0f };
    std::atomic<float> shortTermLufs   { -100.0f };
    std::atomic<float> integratedLufs  { -100.0f };
    std::atomic<float> truePeakDb      { -100.0f };
};
} // namespace duskstudio

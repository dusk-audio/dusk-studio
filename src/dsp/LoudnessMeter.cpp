#include "LoudnessMeter.h"
#include <cmath>

namespace duskstudio
{
namespace
{
// K-weighting biquad coefficients, BS.1770-4.
// Stage 1: high-shelf at ~1500 Hz, +4 dB. Stage 2: high-pass at ~38 Hz.
// Reference values are normalized for 48 kHz in the spec; for arbitrary
// sample rates we re-derive via the bilinear-transform trick described
// in EBU R128 / ITU BS.1770-4 (frequency pre-warping with tan).
//
// Reference Q values come from the spec (Stage 1: 1/sqrt(2) = 0.707;
// Stage 2: ~0.5).
juce::dsp::IIR::Coefficients<float>::Ptr makeKStage1 (double sampleRate)
{
    // High-shelf, +4 dB at 1681 Hz, Q ≈ 0.707.
    return juce::dsp::IIR::Coefficients<float>::makeHighShelf (
        sampleRate, 1681.0, 1.0 / std::sqrt (2.0), juce::Decibels::decibelsToGain (4.0f));
}

juce::dsp::IIR::Coefficients<float>::Ptr makeKStage2 (double sampleRate)
{
    // 2nd-order high-pass, ~38 Hz, Q ≈ 0.5.
    return juce::dsp::IIR::Coefficients<float>::makeHighPass (
        sampleRate, 38.0, 0.5);
}

// Convert mean-square energy to LUFS. BS.1770: L = -0.691 + 10·log10(MS),
// where MS is the channel-weighted mean square (stereo: L=R=1 weighting,
// so just (L_ms + R_ms)).
inline float msToLUFS (double meanSquared)
{
    if (meanSquared <= 1.0e-10) return -100.0f;
    return (float) (-0.691 + 10.0 * std::log10 (meanSquared));
}
} // namespace

LoudnessMeter::LoudnessMeter()
    : oversampler (2 /*channels*/,
                    2 /*stages - 2^2 = 4× oversampling, ITU BS.1770 Annex 2*/,
                    juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                    /*isMaximumQuality*/ true)
{}

void LoudnessMeter::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    blockSize = (int) (sampleRate * 0.1);  // 100 ms
    if (blockSize <= 0) blockSize = 1;

    preparedMaxBlockSize = juce::jmax (1, maxBlockSize);
    oversampleInput.setSize (2, preparedMaxBlockSize, false, false, true);
    oversampler.initProcessing ((size_t) preparedMaxBlockSize);
    oversampler.reset();

    auto s1 = makeKStage1 (sampleRate);
    auto s2 = makeKStage2 (sampleRate);
    kStage1L.coefficients = s1;
    kStage1R.coefficients = s1;
    kStage2L.coefficients = s2;
    kStage2R.coefficients = s2;
    juce::dsp::ProcessSpec spec { sampleRate, 1, 1 };
    kStage1L.prepare (spec); kStage1R.prepare (spec);
    kStage2L.prepare (spec); kStage2R.prepare (spec);

    reset();
}

void LoudnessMeter::reset()
{
    kStage1L.reset(); kStage1R.reset();
    kStage2L.reset(); kStage2R.reset();

    blockSamplesRemaining = blockSize;
    blockSumSquared = 0.0;
    blockHistory.clear();
    blockHistory.reserve (8192);  // ~14 minutes of 100 ms blocks
    for (auto& v : momentaryRingMS)  v = 0.0;
    for (auto& v : shortTermRingMS)  v = 0.0;
    ringWritePos = 0;

    currentTruePeak = 0.0f;
    oversampler.reset();

    momentaryLufs.store (-100.0f, std::memory_order_relaxed);
    shortTermLufs.store (-100.0f, std::memory_order_relaxed);
    integratedLufs.store (-100.0f, std::memory_order_relaxed);
    truePeakDb.store    (-100.0f, std::memory_order_relaxed);
}

void LoudnessMeter::finishBlock()
{
    // Mean squared over this 100 ms block (sum of L^2 + R^2 across both
    // channels, normalized by samples × 2 channels of weight 1.0).
    const double ms = blockSumSquared / juce::jmax (1, blockSize);
    blockHistory.push_back (ms);

    momentaryRingMS [(size_t) (ringWritePos % kMomentaryBlocks)] = ms;
    shortTermRingMS [(size_t) (ringWritePos % kShortTermBlocks)] = ms;
    ++ringWritePos;

    // Sliding-window means.
    auto mean = [] (const double* arr, int n)
    {
        double s = 0.0;
        for (int i = 0; i < n; ++i) s += arr[i];
        return s / n;
    };
    const double mMean = mean (momentaryRingMS, kMomentaryBlocks);
    const double sMean = mean (shortTermRingMS, kShortTermBlocks);
    momentaryLufs.store (msToLUFS (mMean), std::memory_order_relaxed);
    shortTermLufs.store (msToLUFS (sMean), std::memory_order_relaxed);

    // Integrated - gated mean of all blocks. First gate: absolute,
    // -70 LUFS. Then compute mean of those blocks; second gate: relative,
    // -10 LU below the first-pass mean. Final integrated = mean of blocks
    // passing both gates.
    const double absoluteMS = std::pow (10.0, (-70.0 + 0.691) / 10.0);
    double sumPass1 = 0.0;
    int    countPass1 = 0;
    for (auto v : blockHistory)
        if (v > absoluteMS) { sumPass1 += v; ++countPass1; }

    if (countPass1 == 0)
    {
        integratedLufs.store (-100.0f, std::memory_order_relaxed);
    }
    else
    {
        const double meanPass1 = sumPass1 / countPass1;
        const double relativeGateLUFS = msToLUFS (meanPass1) - 10.0;
        const double relativeMS = std::pow (10.0,
                                              (relativeGateLUFS + 0.691) / 10.0);
        const double gateMS = juce::jmax (absoluteMS, relativeMS);

        double sumPass2 = 0.0;
        int    countPass2 = 0;
        for (auto v : blockHistory)
            if (v > gateMS) { sumPass2 += v; ++countPass2; }

        if (countPass2 == 0)
            integratedLufs.store (-100.0f, std::memory_order_relaxed);
        else
            integratedLufs.store (msToLUFS (sumPass2 / countPass2),
                                   std::memory_order_relaxed);
    }

    blockSumSquared = 0.0;
    blockSamplesRemaining = blockSize;
}

void LoudnessMeter::process (const float* L, const float* R, int numSamples) noexcept
{
    if (sr <= 0.0 || L == nullptr || R == nullptr) return;

    // ── K-weighted block accumulation (LUFS) ──
    for (int i = 0; i < numSamples; ++i)
    {
        const float kL = kStage2L.processSample (kStage1L.processSample (L[i]));
        const float kR = kStage2R.processSample (kStage1R.processSample (R[i]));

        blockSumSquared += (double) kL * kL + (double) kR * kR;
        if (--blockSamplesRemaining == 0) finishBlock();
    }

    // ── True-peak detection (4× oversampled) ──
    // Per ITU BS.1770 Annex 2, true-peak is measured on the 4×-upsampled
    // signal. The downsampled output is discarded - we only need the
    // upsampled samples for the peak scan.
    const int n = juce::jmin (numSamples, preparedMaxBlockSize);
    if (n > 0)
    {
        oversampleInput.copyFrom (0, 0, L, n);
        oversampleInput.copyFrom (1, 0, R, n);

        float* channels[2] = { oversampleInput.getWritePointer (0),
                                oversampleInput.getWritePointer (1) };
        juce::dsp::AudioBlock<float> inBlock (channels, 2, (size_t) n);
        auto upBlock = oversampler.processSamplesUp (inBlock);

        const int upN = (int) upBlock.getNumSamples();
        float peak = currentTruePeak;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* p = upBlock.getChannelPointer ((size_t) ch);
            for (int i = 0; i < upN; ++i)
            {
                const float a = std::fabs (p[i]);
                if (a > peak) peak = a;
            }
        }
        currentTruePeak = peak;
    }

    truePeakDb.store (currentTruePeak > 1.0e-5f
                        ? juce::Decibels::gainToDecibels (currentTruePeak, -100.0f)
                        : -100.0f,
                       std::memory_order_relaxed);
}
} // namespace duskstudio

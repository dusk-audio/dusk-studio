#include "LoudnessMeter.h"
#include "../foundation/Decibels.h"

#include <algorithm>
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
duskaudio::BiquadCoeffs makeKStage1 (double sampleRate)
{
    // High-shelf, +4 dB at 1681 Hz, Q ≈ 0.707.
    return duskaudio::Biquad::shelf (sampleRate, 1681.0f, 4.0f,
                                     (float) (1.0 / std::sqrt (2.0)), /*high*/ true);
}

duskaudio::BiquadCoeffs makeKStage2 (double sampleRate)
{
    // 2nd-order high-pass, ~38 Hz, Q ≈ 0.5.
    return duskaudio::Biquad::highPass (sampleRate, 38.0f, 0.5f);
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

LoudnessMeter::LoudnessMeter() = default;

void LoudnessMeter::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    blockSize = (int) (sampleRate * 0.1);  // 100 ms
    if (blockSize <= 0) blockSize = 1;

    preparedMaxBlockSize = std::max (1, maxBlockSize);
    oversampler.setFactor (4);   // ITU BS.1770 Annex 2 true-peak
    oversampler.prepare (preparedMaxBlockSize);

    const auto s1 = makeKStage1 (sampleRate);
    const auto s2 = makeKStage2 (sampleRate);
    kStage1L.setCoeffs (s1); kStage1R.setCoeffs (s1);
    kStage2L.setCoeffs (s2); kStage2R.setCoeffs (s2);

    reset();
}

void LoudnessMeter::reset()
{
    kStage1L.reset(); kStage1R.reset();
    kStage2L.reset(); kStage2R.reset();

    blockSamplesRemaining = blockSize;
    blockSumSquared = 0.0;
    blockHistory.clear();
    blockHistory.reserve ((size_t) kMaxHistoryBlocks);
    for (auto& v : momentaryRingMS)  v = 0.0;
    for (auto& v : shortTermRingMS)  v = 0.0;
    ringWritePos = 0;

    currentTruePeak = 0.0f;
    oversampler.reset();

    momentaryLufs.store (-100.0f, std::memory_order_relaxed);
    shortTermLufs.store (-100.0f, std::memory_order_relaxed);
    integratedLufs.store (-100.0f, std::memory_order_relaxed);
    truePeakDb.store    (-100.0f, std::memory_order_relaxed);
    integratedCapped.store (false, std::memory_order_relaxed);
}

void LoudnessMeter::finishBlock()
{
    // Mean squared over this 100 ms block (sum of L^2 + R^2 across both
    // channels, normalized by samples × 2 channels of weight 1.0).
    const double ms = blockSumSquared / std::max (1, blockSize);
    if (blockHistory.size() < (size_t) kMaxHistoryBlocks)
    {
        blockHistory.push_back (ms);
        if (blockHistory.size() == (size_t) kMaxHistoryBlocks)
            integratedCapped.store (true, std::memory_order_relaxed);
    }

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
        const double gateMS = std::max (absoluteMS, relativeMS);

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

    // K-weighted block accumulation (LUFS)
    for (int i = 0; i < numSamples; ++i)
    {
        const float kL = kStage2L.process (kStage1L.process (L[i]));
        const float kR = kStage2R.process (kStage1R.process (R[i]));

        blockSumSquared += (double) kL * kL + (double) kR * kR;
        if (--blockSamplesRemaining == 0) finishBlock();
    }

    // True-peak detection (4× oversampled)
    // Per ITU BS.1770 Annex 2, true-peak is measured on the 4×-upsampled
    // signal. The downsampled output is discarded - we only need the
    // upsampled samples for the peak scan.
    const int n = std::min (numSamples, preparedMaxBlockSize);
    if (n > 0)
    {
        // Upsample straight from L/R into the oversampler's own scratch; we
        // never downsample, so the FIR state advances but the samples are read
        // once for the peak scan and discarded.
        const auto up = oversampler.processSamplesUp (L, R, n);
        float peak = currentTruePeak;
        for (const float* p : { up.L, up.R })
            for (int i = 0; i < up.numSamples; ++i)
            {
                const float a = std::fabs (p[i]);
                if (a > peak) peak = a;
            }
        currentTruePeak = peak;
    }

    truePeakDb.store (currentTruePeak > 1.0e-5f
                        ? dusk::audio::gainToDecibels (currentTruePeak, -100.0f)
                        : -100.0f,
                       std::memory_order_relaxed);
}
} // namespace duskstudio

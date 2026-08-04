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

inline double lufsToMS (double lufs)
{
    return std::pow (10.0, (lufs + 0.691) / 10.0);
}

// BS.1770-4's absolute gate. Windows below it are excluded for good, so they
// are never retained.
const double kAbsoluteGateMS = lufsToMS (-70.0);
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

    windowMS.assign ((size_t) kMaxWindows, 0.0);

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
    windowCount       = 0;
    absoluteGateSum   = 0.0;
    absoluteGateCount = 0;
    scanGateMS = scanSum = scanCredit = 0.0;
    scanCount = scanPos = scanLimit = 0;
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

void LoudnessMeter::startRelativeScan() noexcept
{
    // Relative gate: 10 LU below the mean of everything above the absolute
    // gate. Both gates apply, so the effective threshold is the higher one.
    // The mean is exact, so the threshold is too - only the window set below
    // has to be recomputed.
    const double meanPass1 = absoluteGateSum / absoluteGateCount;
    scanGateMS = std::max (kAbsoluteGateMS,
                            lufsToMS ((double) msToLUFS (meanPass1) - 10.0));
    scanSum    = 0.0;
    scanCredit = 0.0;
    scanCount  = 0;
    scanPos    = 0;
    scanLimit  = windowCount;
}

void LoudnessMeter::advanceRelativeScan (int numSamples) noexcept
{
    if (scanLimit <= 0) return;

    // Budget the pass against elapsed audio so it lands inside one gating
    // period whatever the buffer size, instead of one callback wearing all of
    // it. Windows past scanLimit were appended mid-pass and wait for the next.
    scanCredit += (double) scanLimit * (double) numSamples
                    / (double) std::max (1, blockSize);
    while (scanPos < scanLimit && scanCredit >= 1.0)
    {
        const double v = windowMS[(size_t) scanPos];
        if (v > scanGateMS)
        {
            scanSum += v;
            ++scanCount;
        }
        ++scanPos;
        scanCredit -= 1.0;
    }

    if (scanPos >= scanLimit)
    {
        integratedLufs.store (scanCount > 0 ? msToLUFS (scanSum / scanCount)
                                            : -100.0f,
                               std::memory_order_relaxed);
        scanLimit = 0;
    }
}

void LoudnessMeter::finishBlock()
{
    // Mean squared over this 100 ms block (sum of L^2 + R^2 across both
    // channels, normalized by samples × 2 channels of weight 1.0).
    const double ms = blockSumSquared / std::max (1, blockSize);

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

    // BS.1770-4 gates 400 ms blocks overlapping by 75 %, i.e. one gating block
    // per 100 ms step once a full window exists. The sub-blocks are equal
    // length, so the mean of the momentary ring IS that window's mean square -
    // gating the 100 ms sub-blocks directly instead discards short dips that
    // the 400 ms window rides over, and disagrees with R128 compliance meters.
    if (ringWritePos >= kMomentaryBlocks && mMean > kAbsoluteGateMS
        && windowCount < kMaxWindows)
    {
        windowMS[(size_t) windowCount] = mMean;
        ++windowCount;
        absoluteGateSum += mMean;
        ++absoluteGateCount;
    }

    if (scanLimit == 0 && absoluteGateCount > 0)
        startRelativeScan();

    blockSumSquared = 0.0;
    blockSamplesRemaining = blockSize;
}

void LoudnessMeter::process (const float* L, const float* R, int numSamples) noexcept
{
    if (sr <= 0.0 || L == nullptr || R == nullptr) return;

    // Deferred requestReset(): applied on this thread so the window and ring
    // wipes never race a concurrent process() pass.
    if (resetRequested.exchange (false, std::memory_order_acquire))
        reset();

    // K-weighted block accumulation (LUFS)
    for (int i = 0; i < numSamples; ++i)
    {
        const float kL = kStage2L.process (kStage1L.process (L[i]));
        const float kR = kStage2R.process (kStage1R.process (R[i]));

        blockSumSquared += (double) kL * kL + (double) kR * kR;
        if (--blockSamplesRemaining == 0) finishBlock();
    }

    advanceRelativeScan (numSamples);

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

    // Close the mid-block request window: finishBlock() and the true-peak
    // publish above may have raced requestReset() after the entry check. Apply
    // that request before returning so stale readings cannot reappear.
    if (resetRequested.exchange (false, std::memory_order_acquire))
        reset();
}
} // namespace duskstudio

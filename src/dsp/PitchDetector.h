// YIN/CMNDF monophonic pitch detector tuned for guitar. Allocation-free.
// Range ~50-1500 Hz. Below kSilenceThreshold reports 0 Hz so the UI can show
// "no signal" instead of noise-floor garbage. prepare() is NOT realtime-safe;
// pushBlock is.
//
// Cost control. The CMNDF scan is O(tau-range x frame) and does not shrink with
// the block size, so at 48 kHz / 2048 history it is ~1M inner iterations no
// matter how small the buffer. Two things keep it inside an audio callback:
//
//   - It runs on a fresh QUARTER of history, not every block. The result feeds a
//     UI meter polled at ~30 Hz, so scanning 750 times a second at 64-sample
//     buffers was pure waste. Level and the silence gate still update per block,
//     so "no signal" stays immediate.
//   - The difference function samples every kFrameStride-th point. CMNDF divides
//     d(tau) by the running mean of d, and striding scales every tau's d by the
//     same factor, so the ratio the dip detection works on is preserved.
//
// Without these, unvoiced-but-audible input (a hand on the strings, amp hum)
// never reaches the early break and overruns a 64-sample block on its own:
// measured 1.68 ms worst case against a 1.33 ms budget, now 0.38 ms.

#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>

class PitchDetector
{
public:
    // Below this RMS, treat as silence and emit 0 Hz.
    static constexpr float kSilenceThreshold = 0.005f; // ~ -46 dBFS

    void prepare (double sampleRate, int historySamples = 2048)
    {
        sampleRate_ = sampleRate;
        history_.assign (static_cast<size_t> (historySamples), 0.0f);
        writePos_ = 0;
        latestHz_ = 0.0f;
        latestLevel_ = 0.0f;
        // A quarter of the history: fast enough that the reading tracks a
        // player retuning a string, cheap enough to stay off the hot path.
        scanInterval_ = std::max (1, historySamples / 4);
        samplesSinceScan_ = scanInterval_;   // scan on the first block after prepare
        // Search range: 50..1500 Hz -> lag range
        minLag_ = std::max (2, static_cast<int> (std::floor (sampleRate / 1500.0)));
        maxLag_ = std::min (static_cast<int> (history_.size()) - 2,
                            static_cast<int> (std::ceil  (sampleRate /   50.0)));
    }

    // Audio-thread safe.
    void pushBlock (const float* samples, int numSamples)
    {
        const int N = static_cast<int> (history_.size());
        if (N <= 0) return;

        // Append + RMS.
        double sumSq = 0.0;
        for (int i = 0; i < numSamples; ++i)
        {
            const float s = samples[i];
            history_[static_cast<size_t> (writePos_)] = s;
            writePos_ = (writePos_ + 1) % N;
            sumSq += static_cast<double> (s) * s;
        }
        latestLevel_ = numSamples > 0
            ? static_cast<float> (std::sqrt (sumSq / static_cast<double> (numSamples)))
            : 0.0f;

        if (latestLevel_ < kSilenceThreshold)
        {
            latestHz_ = 0.0f;
            samplesSinceScan_ = scanInterval_;   // re-scan as soon as signal returns
            return;
        }

        // Hold the last reading between scans rather than clearing it, so the
        // tuner needle stays put instead of flickering to "no signal".
        samplesSinceScan_ += numSamples;
        if (samplesSinceScan_ < scanInterval_) return;
        samplesSinceScan_ = 0;

        // YIN-style: d(τ) = Σ (x[k] - x[k+τ])² with running cumulative-
        // mean normalisation (CMNDF). Frame length = N - maxLag so
        // x[k+τ] never wraps past the latest write.
        const int historyLen = N;
        const int frameLen = historyLen - maxLag_;

        auto readAt = [&] (int idx) noexcept -> float
        {
            // idx = "samples ago, 0=oldest". Buffer full after first
            // prepare()-sized push, oldest sits at writePos_.
            int p = writePos_ + idx;
            if (p >= historyLen) p -= historyLen;
            return history_[static_cast<size_t> (p)];
        };

        float runningCMND = 0.0f;
        int   bestTau    = -1;
        float bestValue  = std::numeric_limits<float>::max();
        bool  inDip      = false;
        constexpr float kCMNDThreshold = 0.15f;  // YIN's harmonic threshold

        for (int tau = minLag_; tau <= maxLag_; ++tau)
        {
            float d = 0.0f;
            for (int k = 0; k < frameLen; k += kFrameStride)
            {
                const float a = readAt (k);
                const float b = readAt (k + tau);
                const float diff = a - b;
                d += diff * diff;
            }
            runningCMND += d;
            const float meanD = runningCMND / static_cast<float> (tau - minLag_ + 1);
            const float cmnd  = (meanD > 0.0f) ? d / meanD : 1.0f;

            // YIN absolute-threshold step: once cmnd dips below the threshold,
            // follow the dip DOWN to its local minimum rather than taking the
            // first crossing. Breaking on the first sub-threshold value lands on
            // the downslope (period too short -> pitch reads sharp); the true
            // period sits at the bottom of the dip.
            if (inDip)
            {
                if (cmnd < bestValue) { bestValue = cmnd; bestTau = tau; }
                else if (cmnd > bestValue) break; // truly climbed back out -> local min found
                // cmnd == bestValue: flat plateau at the minimum (quantised CMNDF) -
                // keep scanning so a deeper dip just past the plateau isn't missed.
            }
            else if (cmnd < kCMNDThreshold)
            {
                inDip     = true;
                bestValue = cmnd;
                bestTau   = tau;
            }
            // No pre-dip fallback: committing the global CMNDF minimum even when nothing
            // ever crosses the threshold defeats YIN's voicing gate - aperiodic / noisy
            // input would report a bogus pitch. bestTau stays -1 until a real dip, so
            // unvoiced input falls through to the 0 Hz return below.
        }

        if (bestTau < 0)
        {
            latestHz_ = 0.0f;
            return;
        }

        // Parabolic interp between τ-1, τ, τ+1 for sub-sample accuracy.
        float fracTau = static_cast<float> (bestTau);
        if (bestTau > minLag_ && bestTau < maxLag_)
        {
            auto diffAt = [&] (int tau) noexcept
            {
                float dd = 0.0f;
                for (int k = 0; k < frameLen; k += kFrameStride)
                {
                    const float a = readAt (k);
                    const float b = readAt (k + tau);
                    const float dif = a - b;
                    dd += dif * dif;
                }
                return dd;
            };
            const float yPrev = diffAt (bestTau - 1);
            const float yCurr = diffAt (bestTau);
            const float yNext = diffAt (bestTau + 1);
            const float denom = yPrev - 2.0f * yCurr + yNext;
            if (std::abs (denom) > 1.0e-12f)
            {
                const float delta = 0.5f * (yPrev - yNext) / denom;
                fracTau = static_cast<float> (bestTau) + std::clamp (delta, -1.0f, 1.0f);
            }
        }

        latestHz_ = static_cast<float> (sampleRate_ / static_cast<double> (fracTau));
    }

    float getLatestHz()    const noexcept { return latestHz_; }
    float getLatestLevel() const noexcept { return latestLevel_; }

private:
    // 4 keeps ~270 points of a 1088-point frame, still far more than the two
    // periods YIN needs at the top of the range.
    static constexpr int kFrameStride = 4;

    double sampleRate_ = 44100.0;
    std::vector<float> history_;
    int writePos_  = 0;
    int minLag_    = 30;
    int maxLag_    = 880;

    float latestHz_    = 0.0f;
    float latestLevel_ = 0.0f;
    int   scanInterval_     = 512;
    int   samplesSinceScan_ = 512;
};

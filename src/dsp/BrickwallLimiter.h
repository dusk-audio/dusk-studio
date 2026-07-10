#pragma once

#include "../foundation/StereoOversampler.h"

#include <atomic>
#include <vector>

namespace duskstudio
{
// True-peak lookahead brickwall limiter for the master / mastering chain
// (Pro-L 2 / Waves L2 class). Transparent gain reduction with a hard ceiling
// guarantee on inter-sample peaks.
//
// Signal flow (all RT-safe; buffers + oversampler allocated only in prepare):
//   1. Input drive.
//   2. 4x linear-phase FIR oversampling — the whole limiting process runs at
//      the oversampled rate so inter-sample peaks are actually detected AND
//      controlled (sample-rate-only detection lets ISP through after the DAC
//      reconstruction filter). NOTE: this is an intentional, documented
//      exception to the project's "no per-DSP-unit internal oversampling" rule
//      — a true-peak limiter cannot function without its own oversampling and
//      it is the terminal output stage, so there is no donor saturation
//      downstream to double-oversample.
//   3. Lookahead delay (~2 ms at the oversampled rate).
//   4. Per-sample required gain min(1, ceiling/|peak|), stereo-linked.
//   5. Sliding-window minimum over the lookahead (monotonic deque, O(1)
//      amortized) feeds a one-pole attack / hold / one-pole release smoother.
//      The lookahead lets the gain finish ramping down BEFORE the peak reaches
//      the output; the smoothing keeps the gain trajectory continuous so there
//      is no intermodulation distortion (the cause of the old "compressor"
//      character was an instant gain snap with a single slow release).
//   6. Gain applied at the oversampled rate.
//   7. Downsample 4x.
//   8. Final safety clamp at the ceiling (the smoother carries the load; the
//      clamp only catches sub-dB residual overshoot from the attack lag and
//      the downsampling filter) — this is what makes the ceiling a hard,
//      true-peak guarantee.
class BrickwallLimiter
{
public:
    BrickwallLimiter() = default;

    // Message thread. Sizes the delay + history buffers and the oversampler
    // for the configured lookahead. Must be called before processInPlace.
    void prepare (double sampleRate, int maxBlockSize,
                   double initialLookaheadMs = 2.0);
    void reset() noexcept;

    // Message thread (atomic stores; audio thread reads).
    void setEnabled    (bool e) noexcept           { enabled.store (e, std::memory_order_relaxed); }
    void setInputDriveDb (float dB) noexcept       { inputDrive.store (dB, std::memory_order_relaxed); }
    void setCeilingDb  (float dB) noexcept         { ceilingDb.store (dB, std::memory_order_relaxed); }
    void setReleaseMs  (float ms) noexcept         { releaseMs.store (ms, std::memory_order_relaxed); }
    // 0 Modern, 1 Transparent, 2 Punchy — shapes hold + release around the
    // release knob. Stereo link off = independent per-channel gain reduction.
    void setMode       (int m) noexcept            { mode.store (m, std::memory_order_relaxed); }
    void setStereoLink (bool b) noexcept           { stereoLink.store (b, std::memory_order_relaxed); }
    // Active lookahead, in ms. The delay/deque are pre-allocated for a fixed
    // maximum in prepare(), so this only re-derives the active read offset,
    // window and reported latency. Called every block from the mastering chain
    // (audio thread), so it must stay RT-safe — no allocation, lock or I/O.
    void setLookaheadMs (float ms) noexcept;

    bool  isEnabled() const noexcept    { return enabled.load (std::memory_order_relaxed); }
    float getInputDriveDb() const noexcept { return inputDrive.load (std::memory_order_relaxed); }
    float getCeilingDb() const noexcept { return ceilingDb.load (std::memory_order_relaxed); }
    float getReleaseMs() const noexcept { return releaseMs.load (std::memory_order_relaxed); }
    int   getMode() const noexcept      { return mode.load (std::memory_order_relaxed); }
    bool  getStereoLink() const noexcept { return stereoLink.load (std::memory_order_relaxed); }
    float getLookaheadMs() const noexcept { return lookaheadMs.load (std::memory_order_relaxed); }

    // Audio thread. In-place stereo process; L and R must be at least
    // `numSamples` floats. `numSamples` must not exceed the maxBlockSize passed
    // to prepare() — the oversampler is sized for it (the mastering chain
    // enforces this). When disabled it stays transparent but still applies the
    // (constant) lookahead + oversampling latency so toggling never pops.
    void processInPlace (float* L, float* R, int numSamples) noexcept;

    // Audio-thread metering: peak GR over the last block, in dB (≤ 0).
    float getCurrentGrDb() const noexcept { return currentGrDb.load (std::memory_order_relaxed); }

    // Round-trip latency the limiter introduces, in base-rate samples
    // (oversampling FIR latency + lookahead). The chain forwards this so the
    // host/transport can compensate.
    int getLatencySamples() const noexcept { return latencySamplesBase.load (std::memory_order_relaxed); }

private:
    // Re-derives the active lookahead (read offset, window, latency) from the
    // lookaheadMs atomic. Runs on the audio thread (via setLookaheadMs), so it
    // stays RT-safe. Reads the plain members osRate/maxLookaheadOs/osFactor/
    // osLatencyBase, which only prepare() writes — safe because the engine
    // suspends audio around prepare(), so it never overlaps process().
    void recomputeActiveLookahead() noexcept;

    double sr     = 0.0;
    double osRate = 0.0;
    int    osFactor = 4;

    // Atomic so the message-thread reader (getLatencySamples) and the
    // setup-thread writer (prepare) don't race on a plain int.
    std::atomic<int> latencySamplesBase { 0 };
    int osLatencyBase = 0;   // oversampler FIR latency, base-rate samples

    dusk::audio::StereoOversampler oversampler;

    // Stereo delay line at the OVERSAMPLED rate — circular, sized for the max
    // lookahead. The active read offset (activeLookaheadOs) is variable, so the
    // lookahead can change at runtime without reallocating.
    int maxLookaheadOs = 0;
    int bufLen         = 0;
    std::vector<float> delayL, delayR;
    int writePos = 0;
    std::atomic<int> activeLookaheadOs { 0 };
    // Last laOs the process loop actually ran with. When activeLookaheadOs
    // changes (live lookahead drag), the min-deque is rebuilt forward from
    // empty rather than extended in place over a window whose older samples it
    // already dropped — see processInPlace.
    int lastActiveLaOs = 0;

    // Per-channel monotonic min deque over the active lookahead window
    // (activeLookaheadOs + 1), stored in a fixed ring so the sliding-window
    // minimum is O(1) amortized with no per-sample scan and no allocation.
    // Capacity = max window + 1. Two independent channels so stereo-link can be
    // disabled; when linked both are fed the same (max-of-L/R) target.
    std::vector<float>     dqVal[2];
    std::vector<long long> dqIdx[2];
    int       dqHead[2]  = { 0, 0 };
    int       dqCount[2] = { 0, 0 };
    long long sampleCounter = 0;

    // Per-channel gain smoother (runs at the oversampled rate): instant attack
    // to the lookahead-window minimum, hold, then a program-dependent release.
    float env[2]         = { 1.0f, 1.0f };
    int   holdCounter[2] = { 0, 0 };
    int   holdOs      = 0;
    float lastReleaseMs = -1.0f;
    int   lastMode      = -1;

    // Program-dependent (dual-time-constant) release: the effective release
    // coefficient blends between a fast and a slow one-pole by how deep and
    // sustained the gain reduction is. Brief/shallow GR recovers fast (keeps
    // transients open); deep/sustained GR recovers slow (no pumping on dense,
    // bass-heavy masters). relDepth[c] is the smoothed reduction driving the
    // blend, so a single transient stays fast while a held passage goes slow.
    float releaseCoefFast = 0.0f;
    float releaseCoefSlow = 0.0f;
    float relDepthCoef    = 0.0f;
    float relDepth[2]     = { 0.0f, 0.0f };

    std::atomic<bool>  enabled    { true };
    std::atomic<float> inputDrive { 0.0f };  // dB
    std::atomic<float> ceilingDb  { -0.3f };
    std::atomic<float> releaseMs  { 100.0f };
    std::atomic<int>   mode       { 0 };
    std::atomic<bool>  stereoLink { true };
    std::atomic<float> lookaheadMs { 2.0f };

    mutable std::atomic<float> currentGrDb { 0.0f };
};
} // namespace duskstudio

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>

namespace duskstudio
{
// Lookahead brickwall peak limiter, intended for the master / mastering chain.
//
// Algorithm (sample-accurate, no allocation on the audio thread):
//   1. Each input sample is pushed into a stereo delay line of length
//      `lookaheadSamples` (default 3 ms - long enough that fast transients
//      are caught without smearing).
//   2. For every input sample we compute the *required* gain to keep the
//      peak at or below the ceiling: required = min(1, ceiling / |x|).
//   3. We track the minimum required gain across the entire lookahead
//      window via a simple O(N) scan per sample (N ~ 144 at 48 k / 3 ms,
//      so a few hundred MFLOPS for stereo - trivial; a monotonic deque
//      is the clean follow-up but isn't audibly different).
//   4. The output envelope snaps INSTANTLY down to the window-min when
//      the new input demands it (the lookahead delay means the envelope
//      ramp starts BEFORE the peak hits the output) and releases up to
//      1.0 with a 1-pole release coefficient.
//   5. Output = delayed_input * envelope. Result: hard ceiling guarantee
//      with no ramp overshoot, no artifacts on impulses, and a release
//      curve that breathes naturally on sustained material.
//
// Inter-sample-peak (true peak / ISP) detection is NOT implemented yet -
// peaks are sample-level. ISP is the obvious follow-up before this
// limiter ships to a real release context.
class BrickwallLimiter
{
public:
    BrickwallLimiter() = default;

    // Message thread. Sizes the delay + history buffers for the configured
    // lookahead. Must be called before processInPlace.
    void prepare (double sampleRate, int maxBlockSize,
                   double lookaheadMs = 3.0);
    void reset() noexcept;

    // Message thread (atomic stores; audio thread reads).
    void setEnabled    (bool e) noexcept           { enabled.store (e, std::memory_order_relaxed); }
    void setInputDriveDb (float dB) noexcept       { inputDrive.store (dB, std::memory_order_relaxed); }
    void setCeilingDb  (float dB) noexcept         { ceilingDb.store (dB, std::memory_order_relaxed); }
    void setReleaseMs  (float ms) noexcept         { releaseMs.store (ms, std::memory_order_relaxed); }

    bool  isEnabled() const noexcept    { return enabled.load (std::memory_order_relaxed); }
    float getInputDriveDb() const noexcept { return inputDrive.load (std::memory_order_relaxed); }
    float getCeilingDb() const noexcept { return ceilingDb.load (std::memory_order_relaxed); }
    float getReleaseMs() const noexcept { return releaseMs.load (std::memory_order_relaxed); }

    // Audio thread. In-place stereo process; L and R must be at least
    // `numSamples` floats. Bypasses internally if !enabled (still applies
    // the lookahead delay so toggling the enable doesn't pop - the delay
    // is applied unconditionally).
    void processInPlace (float* L, float* R, int numSamples) noexcept;

    // Audio-thread metering: peak GR over the last block, in dB (≤ 0).
    float getCurrentGrDb() const noexcept { return currentGrDb.load (std::memory_order_relaxed); }

    // Round-trip latency the limiter introduces - UI can subtract this
    // from playhead/seek displays if it wants sample accuracy. For MVP we
    // ignore it; 3 ms is below the visual jitter threshold.
    int getLatencySamples() const noexcept { return lookaheadSamples.load (std::memory_order_relaxed); }

private:
    double sr             = 0.0;
    // Atomic so the message-thread reader (getLatencySamples) and the audio /
    // setup-thread writer (prepare) don't race on a plain int.
    std::atomic<int> lookaheadSamples { 0 };

    // Stereo delay line - circular, sized to lookaheadSamples + 1.
    std::vector<float> delayL, delayR;
    int writePos = 0;

    // Per-sample required-gain history; same indexing as the delay line.
    std::vector<float> gainHistory;

    float envelope    = 1.0f;
    float releaseCoef = 0.0f;

    std::atomic<bool>  enabled    { true };
    std::atomic<float> inputDrive { 0.0f };  // dB
    std::atomic<float> ceilingDb  { -0.3f };
    std::atomic<float> releaseMs  { 100.0f };

    mutable std::atomic<float> currentGrDb { 0.0f };
};
} // namespace duskstudio

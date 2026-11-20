#pragma once

#include <array>
#include <atomic>
#include "BrickwallLimiter.h"
#include "LoudnessMeter.h"
#include "MasteringDigitalEq.h"
#include "PhaseAnalysisFeed.h"
#include "../session/Session.h"

#if DUSKSTUDIO_HAS_DUSK_DSP
  #include "CompStageAlign.h"
  #include "MultiCompStage.h"
#endif

namespace duskstudio
{
// Mastering-stage signal chain: digital EQ -> multiband compressor ->
// brickwall limiter. Reads its parameters from MasteringParams.
//
// Tape saturation is intentionally NOT in this chain. Mastering happens
// AFTER the bounce, where the master tape sat (if engaged in Mixing) is
// already baked in.
class MasteringChain
{
public:
    MasteringChain() = default;

    // Neither the engine's Effect Oversampling nor anything else reaches the
    // chain's rates: the EQ and the limiter run fixed 4x, and the compressor
    // Multi-Comp 2's own 2x.
    void prepare (double sampleRate, int blockSize);
    void bind (const MasteringParams& params) noexcept;

    void processInPlace (float* L, float* R, int numSamples, bool capturePhase = true) noexcept;

    PhaseAnalysisFeed& getPhaseFeed() noexcept { return phaseFeed; }

    // Reset the integrated LUFS history - used when the user clicks
    // "Reset" in the mastering view, or when a new mix is loaded so the
    // integrated reading reflects only the current source.
    void resetLoudness();

    // Message thread, audio quiesced: carry the integrated and true-peak
    // measurement across a prepare, which clears it.
    LoudnessMeter::History saveLoudnessHistory() const { return loudnessMeter.saveHistory(); }
    void restoreLoudnessHistory (const LoudnessMeter::History& history) noexcept;

    // EQ (oversampler), comp and limiter are in series, so their latencies add.
    // The compressor's counts whether it is in or out: out, its place is a plain
    // delay of the same length, so a bounce always trims one lead-in.
    int getLatencySamples() const noexcept
    {
        return digitalEq.getLatencySamples()
             + compLatencySamples.load (std::memory_order_relaxed)
             + limiter.getLatencySamples();
    }

#if DUSKSTUDIO_HAS_DUSK_DSP
    // Multi-Comp 2 in Multiband mode, for the page's panel and the presets.
    // Message thread.
    MultiCompStage& getCompressor() noexcept { return comp; }
#endif
    BrickwallLimiter& getLimiter() noexcept { return limiter; }

    // Spectrum-analyzer feed (post-EQ)
    // The audio thread writes mono-summed post-EQ samples into a free-running
    // ring; the message thread copies the most recent window for the mastering
    // EQ's FFT overlay. Overlapping writes may mix windows; atomic samples
    // keep this approximate UI view race-free, including during prepare.
    int    readScopeLatest (float* dest, int count) const noexcept;
    // Samples pushed into the scope so far. It stands still while no audio callback
    // runs, which tells a buffer left holding the last sound from new sound.
    long long scopeSamplesWritten() const noexcept { return scopeWritePos.load (std::memory_order_acquire); }
    double getScopeSampleRate() const noexcept { return scopeSampleRate.load (std::memory_order_relaxed); }

private:
    const MasteringParams* paramsRef = nullptr;

    BrickwallLimiter      limiter;
    LoudnessMeter         loudnessMeter;
    MasteringDigitalEq    digitalEq;   // replaces the Tube EQ in the master strip
    PhaseAnalysisFeed     phaseFeed;

    void updateEqParameters() noexcept;
    void updateLimiterParameters() noexcept;

    // Post-EQ scope ring (power-of-two length; mask-indexed). Written on the
    // audio thread, read on the message thread - see readScopeLatest.
    static constexpr int kScopeRingSize = 1 << 14;
    static_assert (std::atomic<float>::is_always_lock_free);
    std::array<std::atomic<float>, kScopeRingSize> scopeRing {};
    std::atomic<long long>   scopeWritePos { 0 };
    std::atomic<int>         scopeRingMask { 0 };   // written in prepare, read by both threads
    std::atomic<double>      scopeSampleRate { 44100.0 };
    void pushScope (const float* L, const float* R, int n) noexcept;

#if DUSKSTUDIO_HAS_DUSK_DSP
    MultiCompStage comp { CompStage::Multiband };
    // The compressor's place as a member of a stage whose latency is always on:
    // the plug-in runs only while engaged and fading, a plain delay of its
    // latency stands in for it otherwise, and the two crossfade time-aligned.
    // The clock is snapped on in prepare and never advanced, so its block plays
    // as a fixed stage.
    CompStageClock compClock;
    CompStageAlign compAlign;
#endif

    // The compressor's latency, read once in prepare, for the bounce, which
    // reads it from another thread.
    std::atomic<int> compLatencySamples { 0 };

    int preparedBlockSize = 0;

    // VU-RMS smoother state - 300 ms tau on the audio thread, same as
    // BusStrip / MasterBus. Drives the mastering analog VU.
    double sampleRateForMeter = 44100.0;
    float  vuRmsLinL = 0.0f;
    float  vuRmsLinR = 0.0f;
    // RMS smoothing coefficient precomputed for the prepared block size so the
    // per-block meter path skips std::exp on the common (full-block) callback.
    int    meterBlockSize = 0;
    float  meterRmsAlpha  = 0.0f;
};
} // namespace duskstudio

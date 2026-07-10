#pragma once

#include "../foundation/MidiBuffer.h"

#include <atomic>
#include <cstdint>

namespace duskstudio
{
// Receives MIDI Clock + transport bytes (F8 / FA / FB / FC) from a chosen
// MIDI input and derives a live BPM + rolling state. Audio-thread only -
// the engine feeds it the per-block MidiBuffer captured from the chosen
// input's MidiMessageCollector and reads back the smoothed tempo +
// transport hints on the same thread.
//
// Tempo derivation: each F8 clock pulse stamps the current playhead
// sample. We hold the last N intervals (default N = kAvgWindow) in a
// circular buffer and average them to suppress per-tick jitter; the
// average converts to BPM via BPM = 60 / (intervalSamples / sr) / 24.
// Variance above kFreewheelThreshold (e.g. > 50% drift between
// successive intervals) freezes the current BPM for one tick - covers
// the occasional clock dropout without yanking tempo.
//
// Transport bytes (FA / FB / FC) flip a "rolling" hint atom; the engine
// chooses whether to act on it (a "tempo only" sync mode ignores the
// flag, while a full chase mode drives Transport state).
//
// Output sample rate is required at construction time so the
// sample->seconds conversion is constant. Re-prepare via prepare()
// when the device sample rate changes.
class MidiSyncReceiver
{
public:
    // Number of clock intervals to average for BPM. 24 = 1 quarter note,
    // which smooths over typical inter-tick jitter without lagging too
    // far behind a real tempo change.
    static constexpr int kAvgWindow = 24;

    // If the most recent interval differs from the running average by
    // more than this factor (i.e. > 50 % drift), treat it as a glitch
    // and skip the update. Real tempo changes from a master sequencer
    // are gradual; this threshold rejects single-tick noise without
    // blocking deliberate ramps.
    static constexpr float kJitterRejectFactor = 1.5f;

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        reset();
    }

    // Audio-thread reset (e.g. on user switching sync source). Drops
    // history without freeing memory.
    void reset() noexcept
    {
        writeIdx = 0;
        filled = 0;
        lastClockSample = -1;
        rolling.store (false, std::memory_order_relaxed);
        bpm.store (0.0f, std::memory_order_relaxed);
    }

    // Audio-thread entry. Walks the MidiBuffer for clock / transport
    // bytes only; everything else is ignored. blockStartSample is the
    // engine's absolute playhead at the start of this block - we use
    // it + each event's sample offset to timestamp ticks against a
    // monotonically increasing sample clock.
    void process (const dusk::MidiBuffer& events,
                  std::int64_t blockStartSample) noexcept;

    // Smoothed BPM derived from the recent clock interval. 0 until the
    // averaging window fills. Message-thread safe (relaxed atomic
    // load); UI can poll on a timer.
    float getBpm() const noexcept { return bpm.load (std::memory_order_relaxed); }

    // True between Start/Continue (FA/FB) and Stop (FC). The engine
    // decides whether to chase this or just track the BPM.
    bool isRolling() const noexcept { return rolling.load (std::memory_order_relaxed); }

private:
    double sr = 48000.0;
    std::int64_t lastClockSample = -1;
    std::int64_t intervals[kAvgWindow] {};
    int writeIdx = 0;
    int filled   = 0;
    std::atomic<float> bpm     { 0.0f };
    std::atomic<bool>  rolling { false };
};
} // namespace duskstudio

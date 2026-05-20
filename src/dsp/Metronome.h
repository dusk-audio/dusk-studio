#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace duskstudio
{
// Metronome click generator. Sits in the AudioEngine's main callback and
// mixes a short tone into the master output at every beat boundary while
// the transport is rolling. Beat-1 (downbeat) gets a higher pitch and a
// touch more level; off-beats are quieter.
//
// The class is thread-safe in the same shape as the rest of the DSP code:
// parameters via atomics (set on message thread, read on audio thread),
// process() is audio-thread.
class Metronome
{
public:
    Metronome() = default;

    void prepare (double sampleRate);
    void reset() noexcept;

    // Audio thread.
    //   playheadStartSample - the absolute sample position at the START of
    //     this block (NOT the end). Beat boundaries are detected by comparing
    //     against the previous call's end position; the metronome itself is
    //     stateless across non-contiguous transport jumps (e.g. seeking will
    //     re-anchor it on the next call).
    //   transportRolling - when false, in-flight clicks are still rendered
    //     to completion but no NEW clicks are triggered. Lets a click that
    //     started just before a Stop ring out instead of cutting hard.
    //   forceEnable      - bypasses the user's CLICK toggle. The audio
    //     engine sets this during count-in pre-roll so the click plays
    //     even if the user hasn't manually engaged the metronome.
    void process (juce::int64 playheadStartSample,
                   bool transportRolling,
                   float* L, float* R, int numSamples,
                   bool forceEnable = false) noexcept;

    // Atomic setters - message thread.
    void setEnabled (bool e) noexcept              { enabled.store (e, std::memory_order_relaxed); }
    void setBpm (float bpm) noexcept               { bpm_.store (bpm, std::memory_order_relaxed); }
    void setBeatsPerBar (int n) noexcept           { beatsPerBar.store (n, std::memory_order_relaxed); }
    void setVolumeDb (float dB) noexcept           { volumeDb.store (dB, std::memory_order_relaxed); }

    bool  isEnabled() const noexcept   { return enabled.load (std::memory_order_relaxed); }
    float getBpm() const noexcept      { return bpm_.load (std::memory_order_relaxed); }

private:
    double sr = 0.0;

    std::atomic<bool>  enabled    { false };
    std::atomic<float> bpm_       { 120.0f };
    std::atomic<int>   beatsPerBar { 4 };
    std::atomic<float> volumeDb   { -12.0f };

    // Click envelope state. -1 means no click is currently sounding.
    int   clickPos = -1;
    int   clickLength = 0;
    float clickFreq = 1000.0f;

    // Cached for beat-edge detection.
    juce::int64 lastBeatIdx = std::numeric_limits<juce::int64>::min();
    bool        lastBeatSeeded = false;
};
} // namespace duskstudio

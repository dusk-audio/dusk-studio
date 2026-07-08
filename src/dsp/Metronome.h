#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <limits>

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
    void process (std::int64_t playheadStartSample,
                   bool transportRolling,
                   float* L, float* R, int numSamples,
                   bool forceEnable = false) noexcept;

    // Atomic setters - message thread.
    void setEnabled (bool e) noexcept              { enabled.store (e, std::memory_order_relaxed); }
    void setBpm (float bpm) noexcept               { bpm_.store (bpm, std::memory_order_relaxed); }
    void setBeatsPerBar (int n) noexcept           { beatsPerBar.store (n, std::memory_order_relaxed); }
    void setVolumeDb (float dB) noexcept           { volumeDb.store (dB, std::memory_order_relaxed); }
    // When true, a fresh click can trigger before the previous click
    // finishes — both bodies render together. When false (default), a
    // new beat boundary cuts the previous click off (legacy mono
    // behaviour, used at typical BPMs where click bodies always end
    // before the next beat anyway).
    void setPolyphonic (bool p) noexcept           { polyphonic.store (p, std::memory_order_relaxed); }

    bool  isEnabled() const noexcept   { return enabled.load (std::memory_order_relaxed); }
    float getBpm() const noexcept      { return bpm_.load (std::memory_order_relaxed); }

private:
    double sr = 0.0;

    std::atomic<bool>  enabled    { false };
    std::atomic<float> bpm_       { 120.0f };
    std::atomic<int>   beatsPerBar { 4 };
    std::atomic<float> volumeDb   { -12.0f };
    std::atomic<bool>  polyphonic { false };

    // Click envelope state. Monophonic path uses [0]; polyphonic mode
    // round-robins through the full ring so a fast-tempo or fast-
    // beat-subdivision click sequence can overlap. kVoices=4 is
    // plenty: at 240 BPM, beat spacing = 250 ms; click body fades
    // out in ~80 ms so 4 simultaneous voices covers extreme cases
    // (e.g. polyrhythm subdivisions) without sounding stacked.
    static constexpr int kVoices = 4;
    struct Voice
    {
        int   pos = -1;          // -1 = idle
        int   length = 0;
        float freq = 1000.0f;
    };
    std::array<Voice, kVoices> voices {};
    int nextVoice = 0;
    // Mono-mode legacy fields retained for source-compat with anything
    // still reading them (cleared on every triggerClick).
    int   clickPos = -1;
    int   clickLength = 0;
    float clickFreq = 1000.0f;

    // Cached for beat-edge detection.
    std::int64_t lastBeatIdx = std::numeric_limits<std::int64_t>::min();
    bool        lastBeatSeeded = false;
};
} // namespace duskstudio

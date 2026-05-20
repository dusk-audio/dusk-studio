#pragma once

#include <juce_core/juce_core.h>
#include <atomic>

namespace duskstudio
{
// Phase 2 minimum: Stopped / Playing / Recording. Punch-in/out + Loop arrive
// in Phase 3 per the spec. The state itself is a single atomic so the audio
// thread can read it lock-free; transitions happen on the message thread.
class Transport
{
public:
    enum class State : int { Stopped = 0, Playing = 1, Recording = 2 };

    State getState() const noexcept { return state.load (std::memory_order_relaxed); }
    bool isStopped()   const noexcept { return getState() == State::Stopped; }
    bool isPlaying()   const noexcept { return getState() == State::Playing; }
    bool isRecording() const noexcept { return getState() == State::Recording; }

    void setState (State s) noexcept { state.store (s, std::memory_order_relaxed); }

    juce::int64 getPlayhead() const noexcept { return playheadSamples.load (std::memory_order_relaxed); }
    void setPlayhead (juce::int64 s) noexcept { playheadSamples.store (s, std::memory_order_relaxed); }

    // Called from the audio callback when state is Playing or Recording.
    void advancePlayhead (int numSamples) noexcept
    {
        playheadSamples.fetch_add (numSamples, std::memory_order_relaxed);
    }

    // Loop region. Only honoured by the audio thread when loopEnabled is true
    // and loopEnd > loopStart. Wrap-around is applied during Playing only -
    // recording stays linear so the captured WAV maps cleanly onto the
    // timeline (loop-take-stacking is a future feature).
    bool        isLoopEnabled() const noexcept    { return loopEnabled.load (std::memory_order_relaxed); }
    void        setLoopEnabled (bool e) noexcept  { loopEnabled.store (e, std::memory_order_relaxed); }
    juce::int64 getLoopStart() const noexcept     { return loopStart.load (std::memory_order_relaxed); }
    juce::int64 getLoopEnd() const noexcept       { return loopEnd.load (std::memory_order_relaxed); }
    void        setLoopRange (juce::int64 s, juce::int64 e) noexcept
    {
        loopStart.store (s, std::memory_order_relaxed);
        loopEnd.store   (e, std::memory_order_relaxed);
    }

    // Punch-in / punch-out window. While recording with punchEnabled, the
    // audio engine only commits samples in [punchIn, punchOut) to the per-track
    // writers. Audio outside the window passes through monitoring as usual but
    // is not written to disk.
    bool        isPunchEnabled() const noexcept    { return punchEnabled.load (std::memory_order_relaxed); }
    void        setPunchEnabled (bool e) noexcept  { punchEnabled.store (e, std::memory_order_relaxed); }
    juce::int64 getPunchIn() const noexcept        { return punchIn.load (std::memory_order_relaxed); }
    juce::int64 getPunchOut() const noexcept       { return punchOut.load (std::memory_order_relaxed); }
    void        setPunchRange (juce::int64 s, juce::int64 e) noexcept
    {
        punchIn.store  (s, std::memory_order_relaxed);
        punchOut.store (e, std::memory_order_relaxed);
    }

private:
    std::atomic<State>       state            { State::Stopped };
    std::atomic<juce::int64> playheadSamples  { 0 };

    std::atomic<bool>        loopEnabled      { false };
    std::atomic<juce::int64> loopStart        { 0 };
    std::atomic<juce::int64> loopEnd          { 0 };

    std::atomic<bool>        punchEnabled     { false };
    std::atomic<juce::int64> punchIn          { 0 };
    std::atomic<juce::int64> punchOut         { 0 };
};
} // namespace duskstudio

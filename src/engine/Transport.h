#pragma once

#include <cstdint>
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

    std::int64_t getPlayhead() const noexcept { return playheadSamples.load (std::memory_order_relaxed); }
    void setPlayhead (std::int64_t s) noexcept { playheadSamples.store (s, std::memory_order_relaxed); }

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
    std::int64_t getLoopStart() const noexcept     { return loopStart.load (std::memory_order_relaxed); }
    std::int64_t getLoopEnd() const noexcept       { return loopEnd.load (std::memory_order_relaxed); }
    void        setLoopRange (std::int64_t s, std::int64_t e) noexcept
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
    std::int64_t getPunchIn() const noexcept        { return punchIn.load (std::memory_order_relaxed); }
    std::int64_t getPunchOut() const noexcept       { return punchOut.load (std::memory_order_relaxed); }
    void        setPunchRange (std::int64_t s, std::int64_t e) noexcept
    {
        punchIn.store  (s, std::memory_order_relaxed);
        punchOut.store (e, std::memory_order_relaxed);
    }

private:
    std::atomic<State>       state            { State::Stopped };
    std::atomic<std::int64_t> playheadSamples  { 0 };

    std::atomic<bool>        loopEnabled      { false };
    std::atomic<std::int64_t> loopStart        { 0 };
    std::atomic<std::int64_t> loopEnd          { 0 };

    std::atomic<bool>        punchEnabled     { false };
    std::atomic<std::int64_t> punchIn          { 0 };
    std::atomic<std::int64_t> punchOut         { 0 };
};
} // namespace duskstudio

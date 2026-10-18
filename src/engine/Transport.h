#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <limits>

namespace duskstudio
{
class Transport
{
public:
    enum class State : int { Stopped = 0, Playing = 1, Recording = 2 };

    State getState() const noexcept { return state.load (std::memory_order_relaxed); }
    bool isStopped()   const noexcept { return getState() == State::Stopped; }
    bool isPlaying()   const noexcept { return getState() == State::Playing; }
    bool isRecording() const noexcept { return getState() == State::Recording; }

    void setState (State s) noexcept { state.store (s, std::memory_order_relaxed); }

    std::int64_t getPlayhead() const noexcept
    {
        const auto requested = pendingLocate.load (std::memory_order_acquire);
        return requested != kNoPendingLocate ? requested : getCommittedPlayhead();
    }
    std::int64_t getCommittedPlayhead() const noexcept
    {
        return playheadSamples.load (std::memory_order_relaxed);
    }

    // Explicit positioning requires a quiescent audio owner.
    void setPlayhead (std::int64_t s) noexcept
    {
        playheadSamples.store (s, std::memory_order_relaxed);
        pendingLocate.store (kNoPendingLocate, std::memory_order_release);
    }

    // A seek is visible to the UI immediately; only the audio owner consumes it.
    void requestLocate (std::int64_t s) noexcept
    {
        if (isPlaying()) setRollStart (s);
        locates.fetch_add (1, std::memory_order_relaxed);
        pendingLocate.store (s, std::memory_order_release);
    }
    std::int64_t beginAudioBlock() noexcept
    {
        const auto requested = pendingLocate.exchange (kNoPendingLocate, std::memory_order_acq_rel);
        if (requested != kNoPendingLocate)
            playheadSamples.store (requested, std::memory_order_relaxed);
        return getCommittedPlayhead();
    }
    void finishAudioBlock (std::int64_t next) noexcept
    {
        playheadSamples.store (next, std::memory_order_relaxed);
    }

    // Where playback or the take last began from a stopped transport, leaving
    // out count-in and pre-roll. Stop returns here under
    // StopBehavior::ReturnToRollStart.
    std::int64_t getRollStart() const noexcept { return rollStart.load (std::memory_order_relaxed); }
    void setRollStart (std::int64_t s) noexcept { rollStart.store (s, std::memory_order_relaxed); }

    // A seek the user asked for. During playback it also becomes the roll
    // start, so Stop comes back to the last place the user sent the playhead.
    // A take keeps its start: seeking while recording leaves the roll start.
    std::function<void (std::int64_t)> onLocate;
    void locate (std::int64_t s)
    {
        if (onLocate) onLocate (s);
        else commitLocate (s);
    }
    void commitLocate (std::int64_t s) noexcept
    {
        setPlayhead (s);
        if (isPlaying())
            setRollStart (s);
        locates.fetch_add (1, std::memory_order_relaxed);
    }

    // How many seeks locate() has made. An automation pass ends when this
    // changes, so the stretch a seek skipped keeps what it held.
    std::uint32_t getLocateCount() const noexcept { return locates.load (std::memory_order_relaxed); }

    bool        isLoopEnabled() const noexcept    { return loopEnabled.load (std::memory_order_relaxed); }
    void        setLoopEnabled (bool e) noexcept  { loopEnabled.store (e, std::memory_order_relaxed); }
    std::int64_t getLoopStart() const noexcept     { return loopStart.load (std::memory_order_relaxed); }
    std::int64_t getLoopEnd() const noexcept       { return loopEnd.load (std::memory_order_relaxed); }
    void        setLoopRange (std::int64_t s, std::int64_t e) noexcept
    {
        loopStart.store (s, std::memory_order_relaxed);
        loopEnd.store   (e, std::memory_order_relaxed);
    }
    // A loop bracket placed by hand: loop is on exactly when the pair makes a
    // range, so in-then-out needs no separate L press.
    void        placeLoopRange (std::int64_t s, std::int64_t e) noexcept
    {
        setLoopRange (s, e);
        setLoopEnabled (e > s);
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
    // A punch bracket placed by hand: punch is armed exactly when the pair
    // makes a range. A pair collapsed to one point disarms, because that
    // range would record nothing.
    void        placePunchRange (std::int64_t s, std::int64_t e) noexcept
    {
        setPunchRange (s, e);
        setPunchEnabled (e > s);
    }

private:
    static constexpr auto kNoPendingLocate = std::numeric_limits<std::int64_t>::min();
    std::atomic<State>       state            { State::Stopped };
    std::atomic<std::int64_t> playheadSamples  { 0 };
    std::atomic<std::int64_t> pendingLocate    { kNoPendingLocate };
    std::atomic<std::int64_t> rollStart        { 0 };
    std::atomic<std::uint32_t> locates         { 0 };

    std::atomic<bool>        loopEnabled      { false };
    std::atomic<std::int64_t> loopStart        { 0 };
    std::atomic<std::int64_t> loopEnd          { 0 };

    std::atomic<bool>        punchEnabled     { false };
    std::atomic<std::int64_t> punchIn          { 0 };
    std::atomic<std::int64_t> punchOut         { 0 };
};
} // namespace duskstudio

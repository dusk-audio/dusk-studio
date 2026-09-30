#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include "audiofile/BufferedFileReader.h"
#include "../foundation/PlanarBuffer.h"
#include "../session/Session.h"

namespace duskstudio
{
class Transport;

// Multi-region playback. One buffered reader per region, each keeping a
// prefetch window resident. Audio thread reads from those windows; on a
// prefetch miss (right after Play or seek) a reader returns silence rather
// than block.
class PlaybackEngine
{
public:
    explicit PlaybackEngine (Session& s);
    ~PlaybackEngine();

    // Pre-allocate scratch for the largest block. Must run before any
    // audio-thread readForTrack so the RT path never allocates.
    void prepare (int maxBlockSize);

    // Lets preparePlayback read the loop range so it can pre-cache the
    // loop-start samples. Called once at AudioEngine construction.
    void bindTransport (const Transport& t) noexcept { transport = &t; }

    // Open readers for every region. Regions sorted by timelineStart so
    // the audio thread can early-out past blocks. Honour plays the session's
    // take audition in place of that track's regions; renders never pass it.
    enum class Audition { Ignore, Honour };
    void preparePlayback (Audition audition = Audition::Ignore);
    void stopPlayback();
    // How many times preparePlayback has run. Message thread; lets a check
    // prove that an edit over many regions rebuilt once.
    std::uint64_t rebuildCount() const noexcept { return rebuilds; }

    // The take the streams play in place of its track's regions, as the last
    // preparePlayback left them; none when they play no audition. Message thread.
    Session::TakeAudition playingAudition() const noexcept
    {
        return { auditionedTrack.load (std::memory_order_relaxed),
                 auditionedTake.load (std::memory_order_relaxed) };
    }

    // Rebuilds one track's streams while the transport rolls, so a comp edit or a
    // take solo is heard at once: the new streams are built and warmed here, and
    // service() hands them to the audio thread once their readers hold the audio
    // at the playhead, or after a short wait regardless. The audio thread then
    // crossfades from the old streams to the new ones. A newer rebuild of the
    // same track replaces one still waiting. Message thread; does nothing unless
    // the streams are live.
    void refreshTrackPlayback (int trackIndex, Audition audition);

    // Hands warmed rebuilds to the audio thread and frees the streams it has
    // finished with. Message thread, called often while the transport rolls.
    void service();

    // Tests only: open every reader without its background thread and fill it
    // at once, so a rebuild is warm the moment it is built.
    void setSynchronousReadersForTest (bool synchronous) noexcept { synchronousReaders = synchronous; }
    // How many track stream sets exist, for tests that prove none leak.
    static int liveStreamCountForTest() noexcept;

    // Hot-update region gain + mute on the live snapshot without
    // rebuilding readers. Matches streams to AudioRegion entries by
    // (file, timelineStart, lengthInSamples) - structural changes
    // (split / join / move) fall through to the next preparePlayback.
    void refreshLiveRegionParams();

    // Sums every active region for `trackIndex` at `playheadSamples`
    // into outL (always written) and outR (nullptr for mono). Regions
    // sum additively for punch crossfades.
    //
    // When loopEnd > loopStart the read is LOOP-AWARE: a block that
    // crosses loopEnd is split at the boundary and the remainder reads
    // from loopStart, matching the transport's post-block wrap, so the
    // seam never plays material past the loop point or skips the loop
    // downbeat. A short raised-cosine declick ramp is applied around
    // each in-block seam. Pass -1/-1 for a plain linear read.
    void readForTrack (int trackIndex, std::int64_t playheadSamples,
                       float* outL, float* outR, int numSamples,
                       std::int64_t loopStart = -1,
                       std::int64_t loopEnd   = -1) noexcept;

private:
    Session& session;
    const Transport* transport = nullptr;
    std::uint64_t rebuilds = 0;

    struct RegionStream
    {
        std::unique_ptr<dusk::audio::BufferedFileReader> reader;
        std::filesystem::path sourcePath;
        std::int64_t timelineStart   = 0;
        std::int64_t lengthInSamples = 0;
        std::int64_t sourceOffset    = 0;
        std::int64_t fadeInSamples   = 0;
        std::int64_t fadeOutSamples  = 0;
        FadeShape   fadeInShape     = FadeShape::Linear;
        FadeShape   fadeOutShape    = FadeShape::Linear;
        // Implicit-crossfade overlap windows in TIMELINE samples. Set
        // after sort: head fades in over overlapPrevLen, tail fades
        // out over overlapNextLen, both EqualPower so summed power
        // stays ~unity. Explicit per-region fades take precedence in
        // their window.
        std::int64_t overlapPrevLen  = 0;
        std::int64_t overlapNextLen  = 0;
        int         numChannels     = 1;
        // gainLinear + muted are plain non-atomic so RegionStream stays
        // movable. refreshLiveRegionParams overwrites them while the
        // audio thread reads; both fields are naturally-aligned and
        // hardware-atomic on supported targets, and one-block stale
        // reads are benign for gain ramps.
        float       gainLinear      = 1.0f;
        bool        muted           = false;

        // Loop-start pre-cache. The reader's window follows playback
        // forward, so the backward seek at every loop wrap misses and
        // returns silence until the prefetch thread catches up - an
        // audible dropout at the top of every cycle. These hold
        // the region's raw source samples for the timeline window
        // [loopCacheTimelineStart, +loopCacheLen), filled on the message
        // thread in preparePlayback while the audio thread is guaranteed
        // out (streamsActive false). The audio thread serves reads from
        // here while the reader re-warms. Stale-guarded: readSpanForTrack
        // only uses the cache when the window matches the loop start it
        // was primed for.
        std::vector<float> loopCacheL, loopCacheR;
        std::int64_t        loopCacheTimelineStart = -1;
        int                loopCacheLen           = 0;
    };

    struct PerTrackStream
    {
        PerTrackStream() noexcept;
        ~PerTrackStream();
        PerTrackStream (const PerTrackStream&) = delete;
        PerTrackStream& operator= (const PerTrackStream&) = delete;

        std::vector<RegionStream> regions;  // sorted by timelineStart
    };

    // One track's streams. While they are live, the audio thread owns `current`
    // and the crossfade state; the message thread only publishes to `incoming`
    // and takes back what the audio thread leaves in `retired`. While they are
    // not, the message thread owns everything.
    struct TrackSlot
    {
        std::atomic<PerTrackStream*> current { nullptr };
        std::atomic<PerTrackStream*> incoming { nullptr };
        std::array<std::atomic<PerTrackStream*>, 4> retired {};
        // The streams being faded out, and how far the fade has run; -1 when
        // none is running. A fade that has run its course but whose streams
        // found no free retired entry holds here until one frees.
        PerTrackStream* fadingFrom = nullptr;
        int             fadePos    = -1;
    };
    std::array<TrackSlot, Session::kNumTracks> slots;

    // A rebuild built and warming, not yet handed to the audio thread.
    struct PendingSwap
    {
        std::unique_ptr<PerTrackStream> stream;
        std::chrono::steady_clock::time_point deadline;
        TakeId auditioned = 0;
    };
    std::array<PendingSwap, Session::kNumTracks> pending;
    bool synchronousReaders = false;

    // One track's streams, readers opened and warmed at `warmAt`, loop cache
    // primed. `auditioned` is set to the take played in place of its regions,
    // or 0. Null when the track has nothing to play.
    std::unique_ptr<PerTrackStream> buildTrackStream (int trackIndex, Audition audition,
                                                      std::int64_t warmAt, TakeId& auditioned);
    void publish (int trackIndex);
    // Whether every reader holds the audio from `playhead` a short way on.
    static bool readersHoldFrom (const PerTrackStream& stream, std::int64_t playhead) noexcept;
    void freeSlot (TrackSlot& slot);

    // One track's streams summed into outL / outR, loop-aware as readForTrack
    // describes. The outputs must be cleared first.
    void readStream (PerTrackStream& stream, std::int64_t playheadSamples, float* outL, float* outR,
                     int numSamples, std::int64_t loopStart, std::int64_t loopEnd) noexcept;

    // The track whose streams play an auditioned take, which
    // refreshLiveRegionParams must leave alone, and that take. Written on the
    // same threads as streams (the message thread, or a render's worker with
    // the device detached), so the editor can read them while a render runs;
    // never read by the audio thread.
    std::atomic<int> auditionedTrack { -1 };
    std::atomic<TakeId> auditionedTake { 0 };

    // One linear (non-wrapping) read span summed into the output at
    // outOffset. The public readForTrack handles clearing, the in-flight
    // guard and loop splitting, then delegates here per span.
    void readSpanForTrack (PerTrackStream& slot, std::int64_t spanStart,
                           float* outL, float* outR, int outOffset,
                           int numSamples) noexcept;

    // Fill one track's loop-start caches for the given loop range. Message
    // thread, before the audio thread can see the streams.
    static void primeLoopCache (PerTrackStream& stream, std::int64_t loopStart, std::int64_t loopEnd);

    // Audio thread bumps audioInFlight BEFORE inspecting streamsActive /
    // streams[] and decrements on exit. stopPlayback clears streamsActive
    // then spins until zero before destroying the readers. Closes the UAF
    // window where a callback that latched Playing is still summing
    // regions while the message thread tears the streams down.
    //
    // The stop's store to streamsActive then load of audioInFlight, against
    // readForTrack's bump then load of streamsActive, is Dekker's pattern, as
    // in RecordManager and MasteringPlayer: all four are seq_cst. With
    // release/acquire each side's load may pass its own store (store
    // buffering), so the stop can read zero while a lane still reads
    // streamsActive true and sums a stream that is being freed.
    std::atomic<bool> streamsActive { false };
    std::atomic<int>  audioInFlight { 0 };

    struct AudioInFlightScope
    {
        std::atomic<int>& c;
        AudioInFlightScope (std::atomic<int>& a) noexcept : c (a)
            { c.fetch_add (1, std::memory_order_seq_cst); }
        ~AudioInFlightScope() noexcept
            { c.fetch_sub (1, std::memory_order_release); }
    };

    dusk::audio::PlanarBuffer readScratch;
    // Where the streams being faded out are read during a crossfade.
    dusk::audio::PlanarBuffer fadeScratch;
};
} // namespace duskstudio

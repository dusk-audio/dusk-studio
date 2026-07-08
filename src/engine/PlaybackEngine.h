#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <memory>
#include "../session/Session.h"

namespace duskstudio
{
class Transport;

// Multi-region playback. One BufferingAudioReader per region, all sharing
// a TimeSliceThread for prefetch. Audio thread reads from pre-filled
// buffers; on prefetch miss (right after Play or seek) readers return
// silence rather than block.
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
    // the audio thread can early-out past blocks.
    void preparePlayback();
    void stopPlayback();

    // Hot-update region gain + mute on the live snapshot without
    // rebuilding readers. Matches streams to AudioRegion entries by
    // (file, timelineStart, lengthInSamples) — structural changes
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
    juce::AudioFormatManager formatManager;

    // Declared before streams so it outlives the readers attached to
    // it (destruction is reverse declaration order).
    juce::TimeSliceThread bufferingThread { "Dusk Studio playback prefetch" };

    struct RegionStream
    {
        std::unique_ptr<juce::BufferingAudioReader> reader;
        juce::File  sourceFile;
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

        // Loop-start pre-cache. BufferingAudioReader prefetches forward
        // only, so the backward seek at every loop wrap misses and (with
        // timeout 0) returns silence until the prefetch thread catches
        // up — an audible dropout at the top of every cycle. These hold
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
        std::vector<RegionStream> regions;  // sorted by timelineStart
    };

    std::array<std::unique_ptr<PerTrackStream>, Session::kNumTracks> streams;

    // One linear (non-wrapping) read span summed into the output at
    // outOffset. The public readForTrack handles clearing, the in-flight
    // guard and loop splitting, then delegates here per span.
    void readSpanForTrack (PerTrackStream& slot, std::int64_t spanStart,
                           float* outL, float* outR, int outOffset,
                           int numSamples) noexcept;

    // Fill every stream's loop-start cache for the given loop range.
    // Message thread, only while streamsActive is false.
    void primeLoopCaches (std::int64_t loopStart, std::int64_t loopEnd);

    // Audio thread bumps audioInFlight BEFORE inspecting streamsActive /
    // streams[] and decrements on exit. stopPlayback clears streamsActive
    // then spins until zero before destroying the readers. Closes the UAF
    // window where a callback that latched Playing is still summing
    // regions while the message thread tears the streams down. Same
    // pattern as RecordManager::audioInFlight.
    std::atomic<bool> streamsActive { false };
    std::atomic<int>  audioInFlight { 0 };

    struct AudioInFlightScope
    {
        std::atomic<int>& c;
        // acq_rel: release publishes the bump to the drain spin; acquire
        // prevents subsequent reads from reordering before the bump.
        // Release-only on decrement is sufficient.
        AudioInFlightScope (std::atomic<int>& a) noexcept : c (a)
            { c.fetch_add (1, std::memory_order_acq_rel); }
        ~AudioInFlightScope() noexcept
            { c.fetch_sub (1, std::memory_order_release); }
    };

    juce::AudioBuffer<float> readScratch;
};
} // namespace duskstudio

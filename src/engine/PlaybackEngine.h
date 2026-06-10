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
    void readForTrack (int trackIndex, juce::int64 playheadSamples,
                       float* outL, float* outR, int numSamples) noexcept;

private:
    Session& session;
    juce::AudioFormatManager formatManager;

    // Declared before streams so it outlives the readers attached to
    // it (destruction is reverse declaration order).
    juce::TimeSliceThread bufferingThread { "Dusk Studio playback prefetch" };

    struct RegionStream
    {
        std::unique_ptr<juce::BufferingAudioReader> reader;
        juce::File  sourceFile;
        juce::int64 timelineStart   = 0;
        juce::int64 lengthInSamples = 0;
        juce::int64 sourceOffset    = 0;
        juce::int64 fadeInSamples   = 0;
        juce::int64 fadeOutSamples  = 0;
        FadeShape   fadeInShape     = FadeShape::Linear;
        FadeShape   fadeOutShape    = FadeShape::Linear;
        // Implicit-crossfade overlap windows in TIMELINE samples. Set
        // after sort: head fades in over overlapPrevLen, tail fades
        // out over overlapNextLen, both EqualPower so summed power
        // stays ~unity. Explicit per-region fades take precedence in
        // their window.
        juce::int64 overlapPrevLen  = 0;
        juce::int64 overlapNextLen  = 0;
        int         numChannels     = 1;
        // gainLinear + muted are plain non-atomic so RegionStream stays
        // movable. refreshLiveRegionParams overwrites them while the
        // audio thread reads; both fields are naturally-aligned and
        // hardware-atomic on supported targets, and one-block stale
        // reads are benign for gain ramps.
        float       gainLinear      = 1.0f;
        bool        muted           = false;
    };

    struct PerTrackStream
    {
        std::vector<RegionStream> regions;  // sorted by timelineStart
    };

    std::array<std::unique_ptr<PerTrackStream>, Session::kNumTracks> streams;

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

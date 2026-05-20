#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <array>
#include <memory>
#include "../session/Session.h"

namespace duskstudio
{
// Multi-region playback. Each track may have any number of AudioRegions on
// the timeline; preparePlayback opens one reader per region. readForTrack
// iterates the track's region list and copies the slice of each region that
// intersects the requested block - supporting overlapping regions (later
// region wins) and gaps (silence) without complicating the audio thread.
//
// Each per-region juce::AudioFormatReader is wrapped in a
// juce::BufferingAudioReader. A shared TimeSliceThread pre-fetches ahead of
// the playhead so the audio thread's read() copies from a pre-filled ring
// instead of doing blocking disk I/O. Read timeout is 0: if the prefetch
// hasn't caught up yet (e.g. immediately after Play, or right after a seek),
// the reader returns silence rather than stall the callback.
class PlaybackEngine
{
public:
    explicit PlaybackEngine (Session& s);
    ~PlaybackEngine();

    // Message thread: pre-allocate the read scratch buffer for the largest
    // block we'll ever see. Must be called before the audio thread starts
    // calling readForTrack so the audio path never allocates.
    void prepare (int maxBlockSize);

    // Message thread: open readers for every region on each track. Regions
    // are sorted by timelineStart so the audio thread can early-out when it
    // passes a region whose start exceeds the current block.
    void preparePlayback();
    // Message thread: close readers.
    void stopPlayback();

    // Audio thread: mix all active regions' audio for `trackIndex` at the
    // given playhead into the output buffer(s). `outL` is always written
    // (cleared first). `outR` is optional — pass nullptr for mono tracks.
    // For stereo regions, both channels are read; for mono regions, the
    // single source channel is duplicated to outR when outR is non-null.
    // Regions sum additively into the output (allowing punch crossfades).
    void readForTrack (int trackIndex, juce::int64 playheadSamples,
                       float* outL, float* outR, int numSamples) noexcept;

private:
    Session& session;
    juce::AudioFormatManager formatManager;

    // Background prefetch thread shared by every per-region BufferingAudioReader.
    // Declared before `streams` so it outlives the readers attached to it
    // (members are destroyed in reverse declaration order - readers detach
    // via removeTimeSliceClient before this thread is torn down).
    juce::TimeSliceThread bufferingThread { "Dusk Studio playback prefetch" };

    struct RegionStream
    {
        // BufferingAudioReader owns the underlying AudioFormatReader and is
        // itself an AudioFormatReader, so readForTrack's read() call site is
        // unchanged. Disk I/O happens on bufferingThread.
        std::unique_ptr<juce::BufferingAudioReader> reader;
        juce::int64 timelineStart   = 0;
        juce::int64 lengthInSamples = 0;
        juce::int64 sourceOffset    = 0;
        juce::int64 fadeInSamples   = 0;
        juce::int64 fadeOutSamples  = 0;
        FadeShape   fadeInShape     = FadeShape::Linear;
        FadeShape   fadeOutShape    = FadeShape::Linear;
        // Implicit-crossfade overlap windows in TIMELINE samples. Set in
        // preparePlayback after sorting: if this region overlaps the previous
        // one in the track's list, overlapPrevStart..timelineStart+0 brackets
        // the head crossfade window for THIS region (we fade in over it).
        // Likewise, if this region overlaps the next one, [end - overlapNextLen,
        // end) brackets the tail (we fade out over it). The shape used for
        // implicit crossfades is FadeShape::EqualPower so summed power stays
        // ~unity across the overlap. Explicit per-region fades (fadeIn/OutSamples
        // > 0) take precedence in their window.
        juce::int64 overlapPrevLen  = 0;  // crossfade head length, samples
        juce::int64 overlapNextLen  = 0;  // crossfade tail length, samples
        int         numChannels     = 1;  // 1 = mono region (duplicate to R when stereo strip),
                                           // 2 = stereo region (read L+R from file).
        // Linear gain factor (dB-converted at preparePlayback so the
        // audio thread doesn't pay for the conversion per sample). 1.0 = unity.
        float       gainLinear      = 1.0f;
        // Snapshot of AudioRegion::muted at preparePlayback time.
        // readForTrack skips muted streams entirely. Toggling
        // AudioRegion::muted while playing takes effect on the next
        // stop+play (i.e. next preparePlayback call); rebuild
        // mid-play is unsafe.
        bool        muted           = false;
    };

    struct PerTrackStream
    {
        std::vector<RegionStream> regions;  // sorted by timelineStart
    };

    std::array<std::unique_ptr<PerTrackStream>, Session::kNumTracks> streams;

    // Pre-allocated mono scratch buffer for AudioFormatReader::read(). Sized
    // once in prepare(); reused on every audio-thread call to readForTrack.
    juce::AudioBuffer<float> readScratch;
};
} // namespace duskstudio

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
// Per-track threaded WAV writer. Lifetime: created on the message thread when
// recording starts, written to from the audio thread (lock-free queue), drained
// by a background TimeSliceThread, and finalized on the message thread when
// recording stops.
class RecordManager
{
public:
    explicit RecordManager (Session& s);
    ~RecordManager();

    // Called on the message thread.
    // Returns false if no tracks are armed.
    bool startRecording (double sampleRate, juce::int64 startSample);

    // Called on the message thread. Closes the writers, finalizes WAV files,
    // appends new AudioRegion entries to each recorded track.
    void stopRecording (juce::int64 endSample);

    // Audio-thread: write `numSamples` of input N for the corresponding armed
    // track. `L` is the left/mono channel; `R` is the right channel for
    // stereo tracks (nullptr for mono). The writer was created with the
    // matching channel count in startRecording; the implementation builds the
    // channel-pointer array to satisfy ThreadedWriter::write's contract that
    // exactly numChannels pointers are non-null. Passing R != nullptr on a
    // mono-armed track is a programming error (asserted; only L is written).
    // Empty blocks (numSamples == 0) early-return without touching the writer.
    void writeInputBlock (int trackIndex,
                            const float* L,
                            const float* R,
                            int numSamples) noexcept;

    // Audio-thread: capture this block's per-track MIDI events. `events`
    // is the post-filter buffer the engine built for this track this block.
    // `blockStartFromRecord` is the sample offset from the recording's
    // start (negative during count-in pre-roll; events with negative
    // sample-positions are dropped at drain time). Lock-free push into a
    // pre-sized ring buffer; drain happens in stopRecording on the message
    // thread.
    void writeMidiBlock (int trackIndex,
                          const juce::MidiBuffer& events,
                          juce::int64 blockStartFromRecord) noexcept;

    bool isActive() const noexcept { return active.load (std::memory_order_relaxed); }

    // Per-armed-track setup failures from the most recent startRecording
    // call. Populated when createOutputStream() returns null (disk full /
    // permission denied / parent dir missing) or when the WAV writer
    // can't be constructed. Empty when every armed track set up cleanly.
    // Message-thread only: caller (TransportBar) reads it immediately
    // after engine.record() returns and surfaces an AlertWindow listing
    // the affected track numbers so the user doesn't lose a take
    // thinking it was captured.
    const std::vector<int>& getLastSetupFailures() const noexcept
    {
        return lastSetupFailures;
    }

    // Mid-take errors latched at stopRecording from the audio-thread
    // counters. WavWrite = ThreadedWriter::write() returned false (disk
    // full / write-error / ring-buffer overrun); MidiOverflow = per-track
    // MIDI FIFO ran out of capacity and dropped events. count is the
    // number of failed writes / dropped events. Empty when the take ran
    // clean. Message-thread only.
    enum class RecordErrorKind { WavWrite, MidiOverflow };
    struct RecordError
    {
        int trackIndex;
        RecordErrorKind kind;
        juce::uint64    count;
    };
    const std::vector<RecordError>& getLastRecordErrors() const noexcept
    {
        return lastRecordErrors;
    }

    // Snapshot of the regions / midiRegions vectors for one track,
    // captured BEFORE and AFTER the most recent stopRecording commit.
    // AudioEngine reads this after stopRecording returns and wraps it
    // in an UndoableAction so Ctrl+Z reverts the recorded take.
    struct TrackCommitDiff
    {
        int                       trackIndex = -1;
        std::vector<AudioRegion>  audioBefore;
        std::vector<AudioRegion>  audioAfter;
        std::vector<MidiRegion>   midiBefore;
        std::vector<MidiRegion>   midiAfter;
    };
    const std::vector<TrackCommitDiff>& getLastCommitDiff() const noexcept
    {
        return lastCommitDiff;
    }
    // Called by AudioEngine after consuming the diff so the next
    // stopRecording starts from a clean slate.
    void clearLastCommitDiff() noexcept { lastCommitDiff.clear(); }

private:
    Session& session;

    struct PerTrackWriter
    {
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        juce::File file;
        juce::int64 framesWritten = 0;
        int numChannels = 1;  // matches the writer's channel count: 1 for mono
                              // tracks, 2 for stereo. Stamped onto the committed
                              // AudioRegion so PlaybackEngine reads back the
                              // right number of channels.
        // Audio-thread counter: ThreadedWriter::write() returned false (disk
        // full / ring overrun / underlying writer failed). Reset at setup,
        // latched into lastRecordErrors at stopRecording.
        std::atomic<juce::uint64> writeFailures { 0 };
    };

    juce::TimeSliceThread diskThread { "Dusk Studio recorder" };
    juce::WavAudioFormat wav;

    std::array<std::unique_ptr<PerTrackWriter>, Session::kNumTracks> writers;

    // Per-track MIDI capture. Audio thread pushes raw events into a fixed-
    // size ring buffer; the message thread drains it in stopRecording and
    // builds a MidiRegion. kCapacity is large enough for ~30 minutes of a
    // very busy controller stream (16 events / sec × 1800 s = 28k events,
    // bounded at 65k). RawEvent is plain-old-data so the FIFO can be
    // pre-sized at startRecording without any heap traffic from the audio
    // thread.
    struct PerTrackMidi
    {
        struct RawEvent
        {
            juce::int64 samplePos = 0;   // from recording start
            juce::uint8 status = 0;
            juce::uint8 data1 = 0;
            juce::uint8 data2 = 0;
        };
        static constexpr int kCapacity = 65536;
        std::vector<RawEvent>  events;
        juce::AbstractFifo     fifo { kCapacity };
        // Audio-thread counter: bumped when an incoming event can't fit
        // because the FIFO is full. Latched into lastRecordErrors at
        // stopRecording so the user is warned about MIDI data loss.
        std::atomic<juce::uint64> overflowCount { 0 };
        PerTrackMidi() : events ((size_t) kCapacity) {}
    };

    std::array<std::unique_ptr<PerTrackMidi>, Session::kNumTracks> midiCaptures;

    // Audio-thread counter for diagnostic logging. Incremented every
    // time writeMidiBlock is called with a non-empty buffer; reset at
    // startRecording, drained + logged at stopRecording. Distinguishes
    // "track wasn't armed in MIDI mode" (counter 0, cap is null)
    // from "track was set up but no events arrived" (counter 0, cap
    // exists) from "events arrived but were filtered out at drain"
    // (counter > drained.size()).
    std::array<std::atomic<int>, Session::kNumTracks> writeMidiBlockCalls {};

    std::atomic<bool> active { false };

    // In-flight counter for the audio-thread entry points
    // (writeInputBlock / writeMidiBlock). The audio thread bumps this
    // BEFORE inspecting `active` or any per-track writer/cap pointer, and
    // decrements when it leaves. stopRecording sets active=false then
    // spins on this until it drains to zero before destroying writers
    // and midiCaptures — closes the use-after-free window where the
    // audio thread could be mid-write while the message thread tears
    // down the ThreadedWriter / FIFO. Yield rather than sleep so the
    // wait stays sub-block in the common case.
    std::atomic<int> audioInFlight { 0 };

    struct AudioInFlightScope
    {
        std::atomic<int>& c;
        // acq_rel on the increment: release publishes the bump to the
        // message thread's drain spin AND acquire prevents subsequent
        // reads (active flag, writer / midiCapture pointers) from being
        // reordered before the bump — without acquire those reads could
        // observe a torn / freed object the message thread had already
        // started tearing down. Release on the decrement is enough: it
        // orders this thread's earlier accesses before the drain sees
        // the count drop to zero.
        AudioInFlightScope (std::atomic<int>& a) noexcept : c (a)
            { c.fetch_add (1, std::memory_order_acq_rel); }
        ~AudioInFlightScope() noexcept
            { c.fetch_sub (1, std::memory_order_release); }
    };

    // Cleared on each startRecording call, populated when an armed
    // track's WAV writer can't be set up. Stored on the message
    // thread only — the audio callback never touches it.
    std::vector<int> lastSetupFailures;

    // Cleared on startRecording, populated at stopRecording by reading
    // the audio-thread counters on each writer / midi capture before
    // teardown. Message-thread only.
    std::vector<RecordError> lastRecordErrors;

    juce::int64 recordStartSample = 0;
    double      recordSampleRate  = 0.0;

    // Populated at stopRecording: per-track snapshot of regions +
    // midiRegions, both BEFORE and AFTER the commit so the engine can
    // wrap it in an undoable transaction.
    std::vector<TrackCommitDiff> lastCommitDiff;
};
} // namespace duskstudio

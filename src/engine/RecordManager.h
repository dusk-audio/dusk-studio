#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <memory>
#include "../session/Session.h"
#include "audiofile/ThreadedFileWriter.h"
#include "audiofile/WriterDrainPool.h"

namespace duskstudio
{
// Per-track threaded WAV writer. Created on the message thread at
// startRecording, written from the audio thread (lock-free ring),
// drained by a shared WriterDrainPool disk thread, finalized on the
// message thread at stopRecording.
class RecordManager
{
public:
    explicit RecordManager (Session& s);
    ~RecordManager();

    // Immutable message-thread snapshot supplied when a loop-record gesture
    // starts. capture is the already punch-intersected portion of the loop;
    // an enabled plan must satisfy loopStart <= captureStart < captureEnd <= loopEnd.
    struct LoopCapturePlan
    {
        bool enabled = false;
        std::int64_t loopStartSample = 0;
        std::int64_t loopEndSample = 0;
        std::int64_t captureStartSample = 0;
        std::int64_t captureEndSample = 0;
    };

    // Fixed-size coordinate handed to all armed-track writes for one split
    // device callback span. transportStart passed to the coordinate method is
    // the timeline position at callbackBaseOffset; inputOffset returned here is
    // absolute in the original callback. Audio advances its channel pointers by
    // inputOffset, while MIDI keeps passing the unsliced original buffer.
    struct LoopCaptureSpan
    {
        int passOrdinal = 0;              // 1-based; 0 is legacy/non-loop
        std::int64_t timelineStart = 0;   // absolute captured position
        int callbackBaseOffset = 0;
        int inputOffset = 0;              // absolute in original callback
        int numSamples = 0;
        bool startsPass = false;
        bool endsPass = false;
    };

    // Message thread. False if no tracks armed. latencyOffsetSamples is
    // subtracted from committed AUDIO region starts (see stopRecording) to
    // compensate for input round-trip delay; the write gate and MIDI
    // placement are unaffected.
    bool startRecording (double sampleRate, std::int64_t startSample,
                          int latencyOffsetSamples = 0);
    bool startRecording (double sampleRate, std::int64_t startSample,
                          int latencyOffsetSamples,
                          const LoopCapturePlan& loopCapturePlan);

    LoopCaptureSpan coordinateLoopCaptureSpan (int passOrdinal,
                                                std::int64_t transportStartSample,
                                                int callbackBaseOffset,
                                                int remainingSamples) const noexcept;
    void beginLoopCaptureSpan (const LoopCaptureSpan& span) noexcept;
    const LoopCapturePlan& getLoopCapturePlan() const noexcept { return loopPlan; }

    // Message thread. Closes writers, finalizes WAV, appends regions.
    void stopRecording (std::int64_t endSample);

    // Audio thread. R == nullptr for mono. numSamples == 0 early-returns.
    void writeInputBlock (int trackIndex,
                            const float* L,
                            const float* R,
                            int numSamples) noexcept;

    // Audio thread. blockStartFromRecord can be negative during count-in
    // pre-roll; events with negative sample positions are dropped at
    // drain time. Lock-free push into a pre-sized ring.
    void writeMidiBlock (int trackIndex,
                          const juce::MidiBuffer& events,
                          std::int64_t blockStartFromRecord) noexcept;

    bool isActive() const noexcept { return active.load (std::memory_order_relaxed); }

    // Populated when createOutputStream returns null (disk full /
    // permission denied / parent dir missing) or the writer can't be
    // constructed. TransportBar surfaces this as an AlertWindow so
    // the user doesn't lose a take silently.
    const std::vector<int>& getLastSetupFailures() const noexcept
    {
        return lastSetupFailures;
    }

    // Mid-take errors latched at stopRecording.
    enum class RecordErrorKind { WavWrite, MidiOverflow, OffsetConsumedTake };
    struct RecordError
    {
        int trackIndex;
        RecordErrorKind kind;
        std::uint64_t    count;
    };
    const std::vector<RecordError>& getLastRecordErrors() const noexcept
    {
        return lastRecordErrors;
    }
    void clearLastRecordErrors() noexcept { lastRecordErrors.clear(); }

    // BEFORE / AFTER snapshots so AudioEngine can wrap stopRecording
    // in an UndoableAction (Ctrl+Z reverts the take).
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
    void clearLastCommitDiff() noexcept { lastCommitDiff.clear(); }

private:
    Session& session;

    static constexpr int kRetainedLoopPasses = 9; // current + eight prior
    struct PassDescriptor
    {
        int passOrdinal = 0;
        std::int64_t timelineStart = 0;
        std::int64_t sourceOffset = 0;
        std::int64_t lengthInSamples = 0;
        bool endsPass = false;
        bool writeFailed = false;
    };

    struct PerTrackWriter
    {
        std::unique_ptr<dusk::audio::ThreadedFileWriter> writer;
        juce::File file;
        std::int64_t framesWritten = 0;
        int numChannels = 1;
        std::atomic<std::uint64_t> writeFailures { 0 };
        std::array<PassDescriptor, kRetainedLoopPasses> loopPasses {};
        int loopPassCount = 0;
    };

    std::array<std::unique_ptr<PerTrackWriter>, Session::kNumTracks> writers;

    // Declared after the writers so it destructs FIRST: the pool's disk thread
    // joins before any ThreadedFileWriter is torn down, so nothing races the
    // per-writer drainOnce() the pool would otherwise be running.
    dusk::audio::WriterDrainPool drainPool { Session::kNumTracks };

    // ~30 min of busy controller stream (16 events/s × 1800 s = 28k).
    // RawEvent is POD so the FIFO pre-sizes without audio-thread heap.
    struct PerTrackMidi
    {
        struct RawEvent
        {
            std::int64_t samplePos = 0;
            std::uint8_t status = 0;
            std::uint8_t data1 = 0;
            std::uint8_t data2 = 0;
            int passOrdinal = 0;
        };
        // AbstractFifo keeps one sentinel slot, so 65,537 registered slots
        // provide a bounded usable capacity of 65,536 events.
        static constexpr int kCapacity = 65536 + 1;
        std::vector<RawEvent>  events;
        juce::AbstractFifo     fifo { kCapacity };
        std::atomic<std::uint64_t> overflowCount { 0 };
        PerTrackMidi() : events ((size_t) kCapacity) {}
    };

    std::array<std::unique_ptr<PerTrackMidi>, Session::kNumTracks> midiCaptures;

    // Diagnostic - distinguishes "track wasn't armed in MIDI mode"
    // (counter 0, cap null) from "armed but no events arrived"
    // (counter 0, cap exists) from "events filtered out at drain"
    // (counter > drained.size()).
    std::array<std::atomic<int>, Session::kNumTracks> writeMidiBlockCalls {};

    std::atomic<bool> active { false };

    // Audio thread bumps BEFORE inspecting active / writer / midiCapture
    // and decrements on exit. stopRecording clears active then spins
    // here until zero before destroying writers. Closes the UAF window
    // where the audio thread could be mid-write while the message
    // thread tears down. Yield (not sleep) keeps wait sub-block.
    std::atomic<int> audioInFlight { 0 };

    struct AudioInFlightScope
    {
        std::atomic<int>& c;
        // acq_rel: release publishes the bump to the drain spin; acquire
        // prevents subsequent reads from reordering before the bump
        // (without it, those reads could observe a torn / freed object).
        // Release-only on decrement is sufficient.
        AudioInFlightScope (std::atomic<int>& a) noexcept : c (a)
            { c.fetch_add (1, std::memory_order_acq_rel); }
        ~AudioInFlightScope() noexcept
            { c.fetch_sub (1, std::memory_order_release); }
    };

    std::vector<int> lastSetupFailures;
    std::vector<RecordError> lastRecordErrors;

    std::int64_t recordStartSample = 0;
    double      recordSampleRate  = 0.0;

    LoopCapturePlan loopPlan;
    LoopCaptureSpan currentLoopSpan;
    std::array<PassDescriptor, kRetainedLoopPasses> loopPasses {};
    int loopPassCount = 0;
    int highestLoopPassOrdinal = 0;
    std::int64_t gestureCapturedAtMs = 0;

    // Subtracted from committed audio region starts; may be negative. The
    // derived region.timelineStart (not the offset) is clamped >= 0 in
    // stopRecording, head-trimming the take. Applied at commit only -
    // count-in and the write gate use recordStartSample.
    int recordLatencyOffsetSamples = 0;

    std::vector<TrackCommitDiff> lastCommitDiff;
};
} // namespace duskstudio

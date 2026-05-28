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
// Per-track threaded WAV writer. Created on the message thread at
// startRecording, written from the audio thread (lock-free queue),
// drained by a TimeSliceThread, finalized on the message thread at
// stopRecording.
class RecordManager
{
public:
    explicit RecordManager (Session& s);
    ~RecordManager();

    // Message thread. False if no tracks armed.
    bool startRecording (double sampleRate, juce::int64 startSample);

    // Message thread. Closes writers, finalizes WAV, appends regions.
    void stopRecording (juce::int64 endSample);

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
                          juce::int64 blockStartFromRecord) noexcept;

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

    struct PerTrackWriter
    {
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        juce::File file;
        juce::int64 framesWritten = 0;
        int numChannels = 1;
        std::atomic<juce::uint64> writeFailures { 0 };
    };

    juce::TimeSliceThread diskThread { "Dusk Studio recorder" };
    juce::WavAudioFormat wav;

    std::array<std::unique_ptr<PerTrackWriter>, Session::kNumTracks> writers;

    // ~30 min of busy controller stream (16 events/s × 1800 s = 28k).
    // RawEvent is POD so the FIFO pre-sizes without audio-thread heap.
    struct PerTrackMidi
    {
        struct RawEvent
        {
            juce::int64 samplePos = 0;
            juce::uint8 status = 0;
            juce::uint8 data1 = 0;
            juce::uint8 data2 = 0;
        };
        static constexpr int kCapacity = 65536;
        std::vector<RawEvent>  events;
        juce::AbstractFifo     fifo { kCapacity };
        std::atomic<juce::uint64> overflowCount { 0 };
        PerTrackMidi() : events ((size_t) kCapacity) {}
    };

    std::array<std::unique_ptr<PerTrackMidi>, Session::kNumTracks> midiCaptures;

    // Diagnostic — distinguishes "track wasn't armed in MIDI mode"
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

    juce::int64 recordStartSample = 0;
    double      recordSampleRate  = 0.0;

    std::vector<TrackCommitDiff> lastCommitDiff;
};
} // namespace duskstudio

#include "RecordManager.h"
#include "audiofile/FileWriter.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <new>
#include <thread>
#include <unordered_map>

namespace duskstudio
{
namespace
{
// Cap on the per-region take history. Each overdub that fully contains
// an existing region pushes the previous take onto previousTakes; without
// a bound, repeated punch-recording in the same spot accumulates
// indefinitely. 8 is plenty for a portastudio retake workflow; older takes
// get trimmed from the back (the oldest, least-likely-to-be-recalled
// entries) when the cap is exceeded.
constexpr int kMaxTakesPerRegion = 8;

template <typename Region>
void trimTakeHistory (Region& region) noexcept
{
    if ((int) region.previousTakes.size() > kMaxTakesPerRegion)
        region.previousTakes.resize ((size_t) kMaxTakesPerRegion);
}

bool sameProvenance (const TakeProvenance& a, const TakeProvenance& b) noexcept
{
    return a.capturedAtMs == b.capturedAtMs
        && a.loopPassOrdinal == b.loopPassOrdinal
        && a.partialPass == b.partialPass;
}

bool sameMidiNote (const MidiNote& a, const MidiNote& b) noexcept
{
    return a.channel == b.channel && a.noteNumber == b.noteNumber
        && a.velocity == b.velocity && a.startTick == b.startTick
        && a.lengthInTicks == b.lengthInTicks;
}

bool sameMidiCc (const MidiCc& a, const MidiCc& b) noexcept
{
    return a.channel == b.channel && a.controller == b.controller
        && a.value == b.value && a.atTick == b.atTick;
}

bool sameAudioTake (const TakeRef& a, const TakeRef& b) noexcept
{
    return a.file == b.file && a.sourceOffset == b.sourceOffset
        && a.lengthInSamples == b.lengthInSamples
        && sameProvenance (a.provenance, b.provenance);
}

bool sameMidiTake (const MidiTakeRef& a, const MidiTakeRef& b) noexcept
{
    return a.lengthInTicks == b.lengthInTicks
        && sameProvenance (a.provenance, b.provenance)
        && a.notes.size() == b.notes.size()
        && a.ccs.size() == b.ccs.size()
        && std::equal (a.notes.begin(), a.notes.end(), b.notes.begin(), sameMidiNote)
        && std::equal (a.ccs.begin(), a.ccs.end(), b.ccs.begin(), sameMidiCc);
}

bool sameAudioRegion (const AudioRegion& a, const AudioRegion& b) noexcept
{
    return a.file == b.file && a.timelineStart == b.timelineStart
        && a.lengthInSamples == b.lengthInSamples && a.sourceOffset == b.sourceOffset
        && a.previousTakes.size() == b.previousTakes.size()
        && sameProvenance (a.provenance, b.provenance)
        && std::equal (a.previousTakes.begin(), a.previousTakes.end(),
                       b.previousTakes.begin(), sameAudioTake);
}

bool sameMidiRegion (const MidiRegion& a, const MidiRegion& b) noexcept
{
    return a.timelineStart == b.timelineStart
        && a.lengthInSamples == b.lengthInSamples
        && a.lengthInTicks == b.lengthInTicks
        && sameProvenance (a.provenance, b.provenance)
        && a.notes.size() == b.notes.size() && a.ccs.size() == b.ccs.size()
        && a.previousTakes.size() == b.previousTakes.size()
        && std::equal (a.notes.begin(), a.notes.end(), b.notes.begin(), sameMidiNote)
        && std::equal (a.ccs.begin(), a.ccs.end(), b.ccs.begin(), sameMidiCc)
        && std::equal (a.previousTakes.begin(), a.previousTakes.end(),
                       b.previousTakes.begin(), sameMidiTake);
}

bool sliceMidiTake (const MidiTakeRef& source,
                    std::int64_t containingSamples,
                    std::int64_t sliceOffsetSamples,
                    std::int64_t sliceLengthSamples,
                    MidiTakeRef& result)
{
    if (containingSamples <= 0 || source.lengthInTicks <= 0
        || sliceOffsetSamples < 0 || sliceLengthSamples <= 0
        || sliceOffsetSamples + sliceLengthSamples > containingSamples)
        return false;

    const auto sampleToTick = [&source, containingSamples] (std::int64_t sample)
    {
        return (std::int64_t) std::llround (
            (double) sample * (double) source.lengthInTicks
            / (double) containingSamples);
    };
    const auto firstTick = std::clamp<std::int64_t> (
        sampleToTick (sliceOffsetSamples), 0, source.lengthInTicks);
    const auto lastTick = std::clamp<std::int64_t> (
        sampleToTick (sliceOffsetSamples + sliceLengthSamples),
        firstTick, source.lengthInTicks);
    if (lastTick <= firstTick)
        return false;

    result = {};
    result.lengthInTicks = lastTick - firstTick;
    result.provenance = source.provenance;
    for (const auto& note : source.notes)
    {
        const auto noteStart = note.startTick;
        const auto noteEnd = note.startTick + note.lengthInTicks;
        const auto overlapStart = std::max (noteStart, firstTick);
        const auto overlapEnd = std::min (noteEnd, lastTick);
        if (overlapEnd <= overlapStart) continue;
        auto sliced = note;
        sliced.startTick = overlapStart - firstTick;
        sliced.lengthInTicks = std::max<std::int64_t> (
            1, overlapEnd - overlapStart);
        result.notes.push_back (sliced);
    }
    for (const auto& cc : source.ccs)
    {
        if (cc.atTick < firstTick || cc.atTick >= lastTick) continue;
        auto sliced = cc;
        sliced.atTick -= firstTick;
        result.ccs.push_back (sliced);
    }
    return true;
}
} // namespace

RecordManager::RecordManager (Session& s) : session (s) {}

RecordManager::~RecordManager()
{
    // Destruction is an abnormal take end: prevent new writes, wait for any
    // audio-thread caller that already entered, then discard the uncommitted
    // capture. The normal stopRecording path mutates the session by creating
    // regions, which must only happen through an explicit engine stop.
    //
    // The wait is deliberately unbounded, unlike stopRecording's capped spin.
    // stopRecording can bail past its cap because it leaves writers[] /
    // midiCaptures[] alive for later reclaim; a destructor cannot - every
    // member (including the audioInFlight atomic the in-flight thread still
    // decrements) is destroyed when it returns, so proceeding would trade
    // the wait for a use-after-free. By this point the engine has detached
    // the audio callback, so the drain is at most one block.
    active.store (false, std::memory_order_release);
    while (audioInFlight.load (std::memory_order_acquire) > 0)
        std::this_thread::yield();

    for (auto& cap : midiCaptures)
        cap.reset();

    for (auto& slot : writers)
    {
        if (slot == nullptr) continue;
        drainPool.remove (slot->writer.get());
        const auto file = slot->file;
        slot.reset();
        file.deleteFile();
    }
}

bool RecordManager::startRecording (double sampleRate, std::int64_t startSample,
                                    int latencyOffsetSamples)
{
    return startRecording (sampleRate, startSample, latencyOffsetSamples,
                           LoopCapturePlan {});
}

bool RecordManager::startRecording (double sampleRate, std::int64_t startSample,
                                    int latencyOffsetSamples,
                                    const LoopCapturePlan& loopCapturePlan)
{
    if (active.load (std::memory_order_relaxed))
        return true;

    // Pairs with the bail-without-teardown path in stopRecording. If
    // the prior take's audio thread is still inside writeInputBlock /
    // writeMidiBlock with cached pointers into writers[] / midiCaptures
    // [], overwriting those slots here would UAF. Refuse to arm until
    // the audio thread drains. In practice this fires only after a
    // real-time-priority disaster on the prior take.
    if (audioInFlight.load (std::memory_order_acquire) > 0)
    {
        std::fprintf (stderr,
                      "[Dusk Studio/RecordManager] startRecording: prior take's audio "
                      "thread still in-flight (audioInFlight=%d); refusing to arm.\n",
                      audioInFlight.load (std::memory_order_relaxed));
        return false;
    }

    if (! session.anyTrackArmed())
    {
        std::fprintf (stderr, "[Dusk Studio/RecordManager] startRecording: anyTrackArmed=false; aborting.\n");
        return false;
    }

    if (loopCapturePlan.enabled
        && (loopCapturePlan.loopStartSample >= loopCapturePlan.loopEndSample
            || loopCapturePlan.captureStartSample < loopCapturePlan.loopStartSample
            || loopCapturePlan.captureStartSample >= loopCapturePlan.captureEndSample
            || loopCapturePlan.captureEndSample > loopCapturePlan.loopEndSample))
    {
        std::fprintf (stderr,
                      "[Dusk Studio/RecordManager] startRecording: enabled loop plan has "
                      "an empty or out-of-loop effective capture; refusing to arm.\n");
        return false;
    }

    auto audioDir = session.getAudioDirectory();
    if (! audioDir.exists())
        audioDir.createDirectory();

    recordStartSample = startSample;
    recordSampleRate  = sampleRate;
    recordLatencyOffsetSamples = latencyOffsetSamples;
    loopPlan = loopCapturePlan;
    currentLoopSpan = {};
    loopPassCount = 0;
    highestLoopPassOrdinal = 0;
    gestureCapturedAtMs = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::system_clock::now().time_since_epoch()).count();

    lastSetupFailures.clear();
    lastRecordErrors.clear();

    // Reset the per-track audio-thread counters before the audio
    // callback can start writing - the counter readout at stopRecording
    // depends on a clean slate per take.
    for (auto& c : writeMidiBlockCalls)
        c.store (0, std::memory_order_relaxed);

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");

    // Tracks whether any track actually got a writer / MIDI capture. If every
    // armed track is skipped (e.g. all frozen), starting would arm a no-op
    // recording that captures nothing - fail instead so the caller can surface it.
    bool anyArmedSetup = false;

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        if (! session.track (t).recordArmed.load (std::memory_order_relaxed))
            continue;

        // Frozen tracks never record: their playback is the baked WAV, so
        // captured MIDI/audio would be silent on playback and silently desync
        // the rendered audio from midiRegions. The arm UI also blocks this, but
        // this is the engine-side backstop for any other arm path (MCU, MIDI
        // bindings). Unfreeze to record.
        if (session.track (t).frozen.load (std::memory_order_relaxed))
            continue;

        // MIDI tracks: spin up the MIDI capture FIFO and skip the WAV
        // writer entirely. The audio thread will push events into the
        // FIFO via writeMidiBlock; stopRecording drains it into a
        // MidiRegion and pushes onto track.midiRegions.
        if (session.track (t).mode.load (std::memory_order_relaxed)
            == (int) Track::Mode::Midi)
        {
            auto cap = std::make_unique<PerTrackMidi>();
            cap->fifo.reset();
            midiCaptures[(size_t) t] = std::move (cap);
            anyArmedSetup = true;
            std::fprintf (stderr,
                          "[Dusk Studio/RecordManager] startRecording: track %d set up MIDI capture "
                          "(midiInputIndex=%d, midiChannel=%d).\n",
                          t + 1,
                          session.track (t).midiInputIndex.load (std::memory_order_relaxed),
                          session.track (t).midiChannel.load (std::memory_order_relaxed));
            continue;
        }

        // No %s through String::formatted: MSVC's wide printf reads a char* as
        // wchar_t* and garbles the name into invalid filename characters - the
        // writer then fails to open and the take is silently dropped.
        auto trackName = "track" + juce::String (t + 1).paddedLeft ('0', 2)
                           + "_" + stamp + ".wav";
        // The stamp has one-second resolution: a stop + re-arm within the
        // same second would collide with the just-committed take, and
        // deleting here would destroy the WAV its region references.
        // getNonexistentChildFile suffixes (2), (3), ... instead.
        juce::File outFile = audioDir.getChildFile (trackName);
        if (outFile.exists())
            outFile = audioDir.getNonexistentChildFile (
                trackName.upToLastOccurrenceOf (".wav", false, true), ".wav");

        // 24-bit WAV per the spec. Channel count follows the track's mode:
        // 1 for Mono / Midi (MIDI tracks don't audio-record yet), 2 for
        // Stereo. The writer's channel count is captured here so writeInput-
        // Block builds a matching channel-pointer array on the audio thread.
        const int trackChannels =
            session.track (t).mode.load (std::memory_order_relaxed)
                == (int) Track::Mode::Stereo ? 2 : 1;

        dusk::audio::WriteSpec spec;
        spec.sampleRate    = sampleRate;
        spec.numChannels   = trackChannels;
        spec.bitsPerSample = 24;
        spec.format        = dusk::audio::WriteSpec::Format::Wav;
        auto fileWriter = dusk::audio::FileWriter::create (
            outFile.getFullPathName().toStdString(), spec);
        if (fileWriter == nullptr)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/RecordManager] startRecording: track %d "
                          "FileWriter::create failed for \"%s\" - take will be silently "
                          "dropped without this surface.\n",
                          t + 1, outFile.getFullPathName().toRawUTF8());
            lastSetupFailures.push_back (t);
            continue;
        }

        // Ring sized to hold ~4 s at the active sample rate so a brief
        // disk stall (NFS hiccup, drive spindown) doesn't push the
        // audio callback into dropped writes. Scaled by sampleRate so a
        // 48k session doesn't pay the 12× memory tax of a 96k worst-
        // case constant: ≈ 1.5 MB / track @ 48k stereo vs ≈ 3 MB @ 96k.
        // Floor at 65536 samples to guarantee a safety margin even on
        // exotic low-rate setups.
        const int kRingFrames = std::max (65536, (int) (sampleRate * 4.0));

        // The ThreadedFileWriter ctor allocates a multi-MB ring per track, the
        // realistic bad_alloc site in this loop on a memory-starved system.
        // Unwind the whole take and return false so record() surfaces the
        // failure; active is still false, so nothing here is visible to the
        // audio thread and the writers built so far can be unregistered and
        // destroyed safely.
        std::unique_ptr<PerTrackWriter> perTrack;
        try
        {
            perTrack = std::make_unique<PerTrackWriter>();
            perTrack->file = outFile;
            perTrack->numChannels = trackChannels;
            perTrack->writer = std::make_unique<dusk::audio::ThreadedFileWriter> (
                std::move (fileWriter), kRingFrames,
                dusk::audio::ThreadedFileWriter::Drain::External);
        }
        catch (const std::bad_alloc&)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/RecordManager] startRecording: track %d "
                          "writer ring allocation failed; aborting the take.\n", t + 1);
            lastSetupFailures.push_back (t);
            for (auto& w : writers)
                if (w != nullptr)
                {
                    drainPool.remove (w->writer.get());
                    w.reset();
                }
            for (auto& cap : midiCaptures)
                cap.reset();
            return false;
        }

        // Registry sized to kNumTracks (never full here); a false return would
        // leave a writer whose ring nothing drains, so drop the take instead.
        if (! drainPool.add (perTrack->writer.get()))
        {
            std::fprintf (stderr,
                          "[Dusk Studio/RecordManager] startRecording: track %d "
                          "drain pool full; take dropped.\n", t + 1);
            lastSetupFailures.push_back (t);
            continue;
        }
        writers[(size_t) t] = std::move (perTrack);
        anyArmedSetup = true;
    }

    if (! anyArmedSetup)
    {
        std::fprintf (stderr, "[Dusk Studio/RecordManager] startRecording: every armed track "
                              "was skipped (frozen, or setup failed); nothing to record.\n");
        return false;
    }

    active.store (true, std::memory_order_release);
    return true;
}

RecordManager::LoopCaptureSpan RecordManager::coordinateLoopCaptureSpan (
    int passOrdinal, std::int64_t transportStartSample,
    int callbackBaseOffset, int remainingSamples) const noexcept
{
    LoopCaptureSpan span;
    // startRecording publishes the immutable plan before its active release;
    // this acquire is the audio-thread half of that handoff.
    if (! active.load (std::memory_order_acquire))
        return span;
    if (! loopPlan.enabled || passOrdinal < 1
        || callbackBaseOffset < 0 || remainingSamples <= 0)
        return span;

    const auto inputOffset64 = std::max<std::int64_t> (
        0, loopPlan.captureStartSample - transportStartSample);
    if (inputOffset64 >= remainingSamples)
        return span;

    const auto capturedStart = transportStartSample + inputOffset64;
    if (capturedStart < loopPlan.captureStartSample
        || capturedStart >= loopPlan.captureEndSample)
        return span;

    const auto available = std::min<std::int64_t> (
        remainingSamples - inputOffset64,
        loopPlan.captureEndSample - capturedStart);
    if (available <= 0)
        return span;

    span.passOrdinal = passOrdinal;
    span.timelineStart = capturedStart;
    span.callbackBaseOffset = callbackBaseOffset;
    span.inputOffset = callbackBaseOffset + (int) inputOffset64;
    span.numSamples = (int) available;
    span.startsPass = capturedStart == loopPlan.captureStartSample;
    span.endsPass = capturedStart + available == loopPlan.captureEndSample;
    return span;
}

void RecordManager::beginLoopCaptureSpan (const LoopCaptureSpan& span) noexcept
{
    AudioInFlightScope guard (audioInFlight);
    if (! active.load (std::memory_order_acquire) || ! loopPlan.enabled)
        return;

    currentLoopSpan = span;
    if (span.passOrdinal < 1 || span.numSamples <= 0)
        return;
    highestLoopPassOrdinal = std::max (highestLoopPassOrdinal, span.passOrdinal);

    PassDescriptor* descriptor = nullptr;
    if (loopPassCount > 0
        && loopPasses[(size_t) (loopPassCount - 1)].passOrdinal == span.passOrdinal)
    {
        descriptor = &loopPasses[(size_t) (loopPassCount - 1)];
    }
    else
    {
        if (loopPassCount == kRetainedLoopPasses)
        {
            std::move (loopPasses.begin() + 1, loopPasses.end(), loopPasses.begin());
            --loopPassCount;
        }
        descriptor = &loopPasses[(size_t) loopPassCount++];
        *descriptor = {};
        descriptor->passOrdinal = span.passOrdinal;
        descriptor->timelineStart = loopPlan.captureStartSample;
    }
    descriptor->lengthInSamples += span.numSamples;
    descriptor->endsPass = descriptor->endsPass || span.endsPass;
}

void RecordManager::stopRecording (std::int64_t endSample)
{
    if (! active.load (std::memory_order_relaxed))
        return;

    active.store (false, std::memory_order_release);

    // Drain in-flight audio-thread calls before reading per-writer
    // counters or destroying writers / midiCaptures. Both writeInputBlock
    // and writeMidiBlock bump audioInFlight before touching their slots;
    // when it reaches zero the audio thread is guaranteed to have left
    // those entry points and any pointers it captured are no longer in
    // use. Yield in a bounded loop - happy-path wait is sub-ms to ~10 ms
    // (one audio block), short enough for the message thread to absorb
    // during a stop transition.
    //
    // Cap at kMaxSpinIterations so a stuck / detached audio thread cannot
    // hang the message thread forever. At a ~1 µs yield cost this is
    // ~1 ms worst-case latency - orders of magnitude over a sane audio
    // block. If we exceed the cap, BAIL the entire teardown: writers[]
    // and midiCaptures[] stay populated, no regions get committed for
    // this take. The audio thread may still be inside writeInputBlock
    // / writeMidiBlock with cached pointers into those slots; tearing
    // them down here would UAF.
    //
    // Recovery: the next startRecording gates itself on audioInFlight
    // == 0 before overwriting writers[]. If the audio thread eventually
    // unstuck (transient scheduling glitch), the slot is reclaimed
    // there. If permanently stuck (real-time priority lost, OS bug),
    // the slot stays leaked until ~RecordManager - better than a UAF
    // crash mid-session.
    constexpr int kMaxSpinIterations = 1000;
    int spinIters = 0;
    while (audioInFlight.load (std::memory_order_acquire) > 0)
    {
        if (++spinIters > kMaxSpinIterations)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/RecordManager] stopRecording: audioInFlight=%d "
                          "after %d yields; BAILING teardown to avoid UAF. Take "
                          "is dropped; writer slots leak until audio thread "
                          "drains (next startRecording will reclaim).\n",
                          audioInFlight.load (std::memory_order_relaxed),
                          kMaxSpinIterations);
            return;
        }
        std::this_thread::yield();
    }

    // Latch audio-thread error counters into lastRecordErrors before
    // teardown so TransportBar can surface them after engine.stop(). Per-
    // writer write failures + per-track MIDI overflows; setup-time
    // failures are tracked separately by lastSetupFailures.
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        if (auto& w = writers[(size_t) t])
        {
            const auto fails = w->writeFailures.load (std::memory_order_relaxed);
            if (fails > 0)
                lastRecordErrors.push_back ({ t, RecordErrorKind::WavWrite, fails });
        }
        if (auto& cap = midiCaptures[(size_t) t])
        {
            const auto over = cap->overflowCount.load (std::memory_order_relaxed);
            if (over > 0)
                lastRecordErrors.push_back ({ t, RecordErrorKind::MidiOverflow, over });
        }
    }

    // Snapshot every track's regions + midiRegions BEFORE the commit so
    // the engine can wrap the diff in an undoable transaction. Per-track
    // entries are only emitted into lastCommitDiff at the END of this
    // method (after the after-snapshot pass) when something actually
    // changed; tracks untouched by the commit are dropped.
    lastCommitDiff.clear();
    std::array<std::vector<AudioRegion>, Session::kNumTracks> beforeAudio;
    std::array<std::vector<MidiRegion>,  Session::kNumTracks> beforeMidi;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        beforeAudio[(size_t) t] = session.track (t).regions;
        beforeMidi[(size_t) t]  = session.track (t).midiRegions.current();
    }

    // Drain any per-track MIDI captures into MidiRegions BEFORE the writer
    // teardown loop below - audio + MIDI commit phases are independent so
    // ordering doesn't matter, but doing MIDI first keeps the two paths
    // visibly separate and the failure cases isolated.
    const float bpm = session.tempoBpm.load (std::memory_order_relaxed);
    const std::int64_t totalSamples = std::max ((std::int64_t) 1, endSample - recordStartSample);
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& cap = midiCaptures[(size_t) t];
        if (cap == nullptr) continue;

        // Drain the lock-free FIFO into a flat vector so we can sort by
        // sample-position before pairing Note On/Off events. Per JUCE's
        // contract events arrive in sample order within a single block,
        // but sample positions across blocks are monotonic so the FIFO
        // order is already correct - we still copy into a vector to allow
        // the linear note-pairing pass below.
        const int avail = cap->fifo.getNumReady();
        std::vector<PerTrackMidi::RawEvent> drained;
        drained.reserve ((size_t) avail);
        int s1 = 0, sz1 = 0, s2 = 0, sz2 = 0;
        cap->fifo.prepareToRead (avail, s1, sz1, s2, sz2);
        for (int i = 0; i < sz1; ++i) drained.push_back (cap->events[(size_t) (s1 + i)]);
        for (int i = 0; i < sz2; ++i) drained.push_back (cap->events[(size_t) (s2 + i)]);
        cap->fifo.finishedRead (sz1 + sz2);

        // Diagnostic: how many MIDI events did we actually capture for
        // this track? 0 means perTrackMidiScratch was empty across the
        // whole take - usually means the track's midiInputIndex was -1
        // (no MIDI input picked in the dropdown), so the per-track filter
        // never copied any events into the scratch even though the chord
        // analyzer (which reads engine-wide perInputMidi) might have
        // shown chords.
        std::fprintf (stderr,
                      "[Dusk Studio/RecordManager] Track %d MIDI capture: %d events drained, "
                      "writeMidiBlock-calls=%d, %d total samples, midiInputIndex=%d\n",
                      t + 1, (int) drained.size(),
                      writeMidiBlockCalls[(size_t) t].load (std::memory_order_relaxed),
                      (int) totalSamples,
                      session.track (t).midiInputIndex.load (std::memory_order_relaxed));

        if (drained.empty())
        {
            if (! loopPlan.enabled)
            {
                cap.reset();
                continue;
            }
        }

        if (loopPlan.enabled)
        {
            struct ActiveNote
            {
                bool active = false;
                int velocity = 0;
                std::int64_t startSample = 0;
            };
            std::array<ActiveNote, 16 * 128> activeNotes {};
            std::array<int, 16 * 128> controllerState {};
            controllerState.fill (-1);
            std::vector<MidiRegion> nonemptyPasses;
            nonemptyPasses.reserve ((size_t) kRetainedLoopPasses);
            const auto captureLength = loopPlan.captureEndSample
                                     - loopPlan.captureStartSample;
            size_t drainedIndex = 0;

            for (int passOrdinal = 1;
                 passOrdinal <= highestLoopPassOrdinal; ++passOrdinal)
            {
                PassDescriptor pass;
                pass.passOrdinal = passOrdinal;
                pass.timelineStart = loopPlan.captureStartSample;
                pass.lengthInSamples = captureLength;
                pass.endsPass = true;
                for (int i = 0; i < loopPassCount; ++i)
                    if (loopPasses[(size_t) i].passOrdinal == passOrdinal)
                    {
                        pass = loopPasses[(size_t) i];
                        break;
                    }
                if (pass.lengthInSamples <= 0) continue;

                MidiRegion passRegion;
                passRegion.timelineStart = loopPlan.captureStartSample;
                passRegion.lengthInSamples = pass.lengthInSamples;
                passRegion.lengthInTicks = samplesToTicks (
                    pass.lengthInSamples, recordSampleRate, bpm);
                passRegion.recordedAtBPM = (double) bpm;
                passRegion.provenance = {
                    gestureCapturedAtMs, pass.passOrdinal,
                    ! pass.endsPass || pass.lengthInSamples < captureLength
                };

                if (passOrdinal > 1)
                {
                    for (int key = 0; key < (int) activeNotes.size(); ++key)
                        if (activeNotes[(size_t) key].active)
                            activeNotes[(size_t) key].startSample = 0;

                    for (int key = 0; key < (int) controllerState.size(); ++key)
                    {
                        const int value = controllerState[(size_t) key];
                        if (value <= 0) continue;
                        MidiCc chased;
                        chased.channel = key / 128 + 1;
                        chased.controller = key % 128;
                        chased.value = value;
                        chased.atTick = 0;
                        passRegion.ccs.push_back (chased);
                    }
                }

                while (drainedIndex < drained.size()
                       && drained[drainedIndex].passOrdinal < pass.passOrdinal)
                    ++drainedIndex;
                while (drainedIndex < drained.size()
                       && drained[drainedIndex].passOrdinal == pass.passOrdinal)
                {
                    const auto& ev = drained[drainedIndex++];
                    if (ev.passOrdinal != pass.passOrdinal
                        || ev.samplePos < 0
                        || ev.samplePos >= pass.lengthInSamples)
                        continue;

                    const int channel = (ev.status & 0x0F) + 1;
                    const int statusType = ev.status & 0xF0;
                    const int noteKey = (channel - 1) * 128 + ev.data1;
                    if (statusType == 0x90 && ev.data2 > 0)
                    {
                        auto& note = activeNotes[(size_t) noteKey];
                        note.active = true;
                        note.velocity = ev.data2;
                        note.startSample = ev.samplePos;
                    }
                    else if (statusType == 0x80
                             || (statusType == 0x90 && ev.data2 == 0))
                    {
                        auto& note = activeNotes[(size_t) noteKey];
                        if (! note.active) continue;
                        MidiNote committed;
                        committed.channel = channel;
                        committed.noteNumber = ev.data1;
                        committed.velocity = note.velocity;
                        committed.startTick = samplesToTicks (
                            note.startSample, recordSampleRate, bpm);
                        const auto offTick = samplesToTicks (
                            ev.samplePos, recordSampleRate, bpm);
                        committed.lengthInTicks = std::max<std::int64_t> (
                            1, offTick - committed.startTick);
                        passRegion.notes.push_back (committed);
                        note.active = false;
                    }
                    else if (statusType == 0xB0)
                    {
                        MidiCc committed;
                        committed.channel = channel;
                        committed.controller = ev.data1;
                        committed.value = ev.data2;
                        committed.atTick = samplesToTicks (
                            ev.samplePos, recordSampleRate, bpm);
                        passRegion.ccs.push_back (committed);
                        controllerState[(size_t) ((channel - 1) * 128 + ev.data1)]
                            = ev.data2;
                    }
                }

                // A held note is closed at every seam and remains active so
                // the following pass begins with an implicit tick-zero
                // retrigger. This keeps each take independently playable.
                for (int key = 0; key < (int) activeNotes.size(); ++key)
                {
                    const auto& note = activeNotes[(size_t) key];
                    if (! note.active) continue;
                    MidiNote committed;
                    committed.channel = key / 128 + 1;
                    committed.noteNumber = key % 128;
                    committed.velocity = note.velocity;
                    committed.startTick = samplesToTicks (
                        note.startSample, recordSampleRate, bpm);
                    committed.lengthInTicks = std::max<std::int64_t> (
                        1, passRegion.lengthInTicks - committed.startTick);
                    passRegion.notes.push_back (committed);
                }

                if (passOrdinal < highestLoopPassOrdinal)
                {
                    for (int key = 0; key < (int) controllerState.size(); ++key)
                    {
                        if (controllerState[(size_t) key] <= 0) continue;
                        MidiCc reset;
                        reset.channel = key / 128 + 1;
                        reset.controller = key % 128;
                        reset.value = 0;
                        reset.atTick = std::max<std::int64_t> (
                            0, passRegion.lengthInTicks - 1);
                        passRegion.ccs.push_back (reset);
                    }
                }

                if (! passRegion.notes.empty() || ! passRegion.ccs.empty())
                {
                    if ((int) nonemptyPasses.size() == kRetainedLoopPasses)
                        nonemptyPasses.erase (nonemptyPasses.begin());
                    nonemptyPasses.push_back (std::move (passRegion));
                }
            }

            if (nonemptyPasses.empty())
            {
                cap.reset();
                continue;
            }

            MidiRegion region = std::move (nonemptyPasses.back());
            for (auto it = nonemptyPasses.rbegin() + 1;
                 it != nonemptyPasses.rend(); ++it)
            {
                region.previousTakes.push_back (makeMidiTakeRef (*it));
                trimTakeHistory (region);
            }

            const auto newStart = region.timelineStart;
            const auto newEnd = newStart + region.lengthInSamples;
            session.track (t).midiRegions.mutate (
                [&region, newStart, newEnd] (std::vector<MidiRegion>& regions)
                {
                    for (auto it = regions.begin(); it != regions.end(); )
                    {
                        const auto existingStart = it->timelineStart;
                        const auto existingEnd = existingStart + it->lengthInSamples;
                        const bool fullyContained = existingStart >= newStart
                                                 && existingEnd <= newEnd;
                        if (fullyContained)
                        {
                            region.previousTakes.push_back (makeMidiTakeRef (*it));
                            for (auto& deeper : it->previousTakes)
                                region.previousTakes.push_back (std::move (deeper));
                            trimTakeHistory (region);
                            it = regions.erase (it);
                            continue;
                        }

                        // A final partial pass may sit inside a longer MIDI
                        // region. Preserve the longer region on the timeline,
                        // but add compatible, range-sliced payloads after the
                        // loop-pass history so cycling remains aligned.
                        if (existingStart <= newStart && existingEnd >= newEnd)
                        {
                            const auto sliceOffset = newStart - existingStart;
                            const auto sliceLength = newEnd - newStart;
                            MidiTakeRef sliced;
                            if (sliceMidiTake (makeMidiTakeRef (*it),
                                               it->lengthInSamples,
                                               sliceOffset, sliceLength, sliced))
                                region.previousTakes.push_back (std::move (sliced));
                            for (const auto& deeper : it->previousTakes)
                                if (sliceMidiTake (deeper, it->lengthInSamples,
                                                   sliceOffset, sliceLength, sliced))
                                    region.previousTakes.push_back (std::move (sliced));
                            trimTakeHistory (region);
                        }
                        ++it;
                    }
                    regions.push_back (std::move (region));
                });

            cap.reset();
            continue;
        }

        MidiRegion region;
        region.timelineStart   = recordStartSample;
        region.lengthInSamples = totalSamples;
        region.lengthInTicks   = samplesToTicks (totalSamples, recordSampleRate, bpm);
        region.recordedAtBPM   = (double) bpm;

        // Pair Note On / Note Off into MidiNote entries. Pending map keyed
        // on (channel, noteNumber) so concurrent notes on different keys
        // don't collide. Vel-0 Note On counts as Note Off (running-status
        // controllers use this convention to save bandwidth).
        struct PendingNote { std::int64_t startSample; int velocity; };
        std::unordered_map<int, PendingNote> pending;
        auto noteKey = [] (int ch, int note) { return ch * 256 + note; };

        for (const auto& ev : drained)
        {
            // Drop events captured before the take's logical start (count-in
            // pre-roll fires the audio callback but the take begins at
            // recordStartSample = activeRecordStart).
            if (ev.samplePos < 0) continue;
            if (ev.samplePos >= totalSamples) continue;

            const int channel = (ev.status & 0x0F) + 1;     // 1..16
            const int statusType = ev.status & 0xF0;

            if (statusType == 0x90 && ev.data2 > 0)         // Note On
            {
                pending[noteKey (channel, ev.data1)] = { ev.samplePos, ev.data2 };
            }
            else if (statusType == 0x80                      // Note Off
                     || (statusType == 0x90 && ev.data2 == 0))
            {
                const auto k = noteKey (channel, ev.data1);
                auto it = pending.find (k);
                if (it == pending.end()) continue;
                MidiNote n;
                n.channel    = channel;
                n.noteNumber = ev.data1;
                n.velocity   = it->second.velocity;
                n.startTick  = samplesToTicks (it->second.startSample, recordSampleRate, bpm);
                const auto offTick = samplesToTicks (ev.samplePos, recordSampleRate, bpm);
                n.lengthInTicks = std::max ((std::int64_t) 1, offTick - n.startTick);
                region.notes.push_back (n);
                pending.erase (it);
            }
            else if (statusType == 0xB0)                     // CC
            {
                MidiCc c;
                c.channel    = channel;
                c.controller = ev.data1;
                c.value      = ev.data2;
                c.atTick     = samplesToTicks (ev.samplePos, recordSampleRate, bpm);
                region.ccs.push_back (c);
            }
            // Other channel-voice messages (pitch bend, aftertouch,
            // program) are dropped for now - the model holds notes + CCs
            // only. Phase 4c can extend MidiCc with a status discriminant
            // or add dedicated event vectors when the piano roll surfaces
            // them.
        }

        // Hanging notes - any Note On still in `pending` had no matching
        // Note Off in the captured stream. Truncate them to the end of
        // the region so the saved data has no dangling state. Real DAWs
        // also do this on punch-out / stop.
        for (const auto& [key, pn] : pending)
        {
            MidiNote n;
            n.channel    = (key / 256);
            n.noteNumber = (key % 256);
            n.velocity   = pn.velocity;
            n.startTick  = samplesToTicks (pn.startSample, recordSampleRate, bpm);
            n.lengthInTicks = std::max ((std::int64_t) 1,
                region.lengthInTicks - n.startTick);
            region.notes.push_back (n);
        }

        if (region.notes.empty() && region.ccs.empty())
        {
            cap.reset();
            continue;
        }

        // Take-history capture, mirrors AudioRegion's fully-contained
        // overdub absorption below. Any existing MIDI region whose
        // timeline range sits fully inside the new take's range gets
        // moved into the new region's previousTakes (with its own
        // deeper history forwarded so an overdub-of-an-overdub doesn't
        // lose grandparent takes). Partial overlaps are intentionally
        // NOT absorbed - the user can still see / cycle to the older
        // takes via the badge UI; partial-overlap merging would need
        // a tick-domain split routine that's out of scope here.
        const std::int64_t newStart = region.timelineStart;
        const std::int64_t newEnd   = newStart + region.lengthInSamples;
        session.track (t).midiRegions.mutate (
            [&region, newStart, newEnd] (std::vector<MidiRegion>& mregs)
            {
                for (auto it = mregs.begin(); it != mregs.end(); )
                {
                    const auto exStart = it->timelineStart;
                    const auto exEnd   = it->timelineStart + it->lengthInSamples;
                    const bool fullyContained = exStart >= newStart && exEnd <= newEnd;
                    if (! fullyContained) { ++it; continue; }

                    region.previousTakes.push_back (makeMidiTakeRef (*it));

                    for (auto& deeper : it->previousTakes)
                        region.previousTakes.push_back (std::move (deeper));

                    trimTakeHistory (region);

                    it = mregs.erase (it);
                }

                mregs.push_back (std::move (region));
            });

        cap.reset();
    }

    // Tear down writers (this flushes the threaded queues and closes the
    // WAV files), then commit a Region for each.
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& slot = writers[(size_t) t];
        if (slot == nullptr) continue;

        const auto frames = slot->framesWritten;
        // Producer stopped (active false + audioInFlight drained above): the
        // pool drains this writer's ring to empty and flushes it on this thread,
        // then unregisters it, so the file is complete before reset() closes it.
        drainPool.remove (slot->writer.get());
        slot->writer.reset();  // flush + close

        // Latency compensation is applied independently to every loop pass.
        // The writer remains one continuous spool; only each take reference's
        // source offset and length are head-trimmed at commit.
        const std::int64_t shifted = (loopPlan.enabled
                                          ? loopPlan.captureStartSample
                                          : recordStartSample)
                                   - recordLatencyOffsetSamples;
        const std::int64_t trim = shifted < 0 ? -shifted : 0;
        AudioRegion region;
        bool hasRegion = false;

        if (loopPlan.enabled)
        {
            std::vector<AudioRegion> committedPasses;
            committedPasses.reserve ((size_t) slot->loopPassCount);
            const auto captureLength = loopPlan.captureEndSample
                                     - loopPlan.captureStartSample;
            for (int i = 0; i < slot->loopPassCount; ++i)
            {
                const auto& pass = slot->loopPasses[(size_t) i];
                if (pass.writeFailed || pass.lengthInSamples <= 0
                    || trim >= pass.lengthInSamples)
                    continue;
                AudioRegion passRegion;
                passRegion.file = slot->file;
                passRegion.timelineStart = std::max<std::int64_t> (0, shifted);
                passRegion.lengthInSamples = pass.lengthInSamples - trim;
                passRegion.sourceOffset = pass.sourceOffset + trim;
                passRegion.numChannels = slot->numChannels;
                passRegion.provenance = {
                    gestureCapturedAtMs, pass.passOrdinal,
                    ! pass.endsPass || pass.lengthInSamples < captureLength
                };
                committedPasses.push_back (std::move (passRegion));
            }

            if (! committedPasses.empty())
            {
                region = std::move (committedPasses.back());
                for (auto it = committedPasses.rbegin() + 1;
                     it != committedPasses.rend(); ++it)
                {
                    region.previousTakes.push_back (makeAudioTakeRef (*it));
                    trimTakeHistory (region);
                }
                hasRegion = true;
            }
        }
        else if (frames > 0 && trim < frames)
        {
            region.file = slot->file;
            region.timelineStart = std::max<std::int64_t> (0, shifted);
            region.lengthInSamples = frames - trim;
            region.sourceOffset = trim;
            region.numChannels = slot->numChannels;
            hasRegion = true;
        }

        if (hasRegion)
        {
            // Take-history capture: any existing region whose timeline range
            // is FULLY CONTAINED within the new take's range gets absorbed
            // into previousTakes. The user can then cycle through them via
            // the badge UI without losing access to earlier takes.
            //
            // Partial overlaps (e.g. punch-in over the middle of a longer
            // take) are not absorbed wholesale: Pass 2 retains their outer
            // fragments and saves the overwritten compatible slice as a take.
            const std::int64_t newStart = region.timelineStart;
            const std::int64_t newEnd   = newStart + region.lengthInSamples;
            auto& regs = session.track (t).regions;

            // Crossfade length: 64 samples per side, raised-cosine
            // shape. DuskStudio.md §5b specifies the click-mask fade as
            // 64 samples ~ 1.3 ms at 48 kHz - imperceptible as a fade
            // but enough to suppress the boundary discontinuity. Bound
            // by half the new take's length so a punch shorter than 128
            // samples still gets symmetric ramps that don't overlap.
            constexpr std::int64_t kPunchFadeSamples = 64;
            const std::int64_t fadeSamples = std::max (
                (std::int64_t) 0, std::min (kPunchFadeSamples, region.lengthInSamples / 2));

            // Pass 1 - fully-contained takes get absorbed into the new
            // region's previousTakes (no audio overlap, just history).
            // Partial overlaps fall through to Pass 2 below.
            std::vector<AudioRegion> spawnedFragments;
            for (auto it = regs.begin(); it != regs.end(); )
            {
                const auto exStart = it->timelineStart;
                const auto exEnd   = it->timelineStart + it->lengthInSamples;
                const bool fullyContained = exStart >= newStart && exEnd <= newEnd;
                if (! fullyContained) { ++it; continue; }

                region.previousTakes.push_back (makeAudioTakeRef (*it));

                // Carry forward the displaced region's own history so we
                // don't drop deeper takes when overdubbing repeatedly. The
                // newly-displaced take goes first, then the older ones.
                for (auto& deeper : it->previousTakes)
                    region.previousTakes.push_back (std::move (deeper));

                trimTakeHistory (region);

                it = regs.erase (it);
            }

            // Pass 2 - partial overlaps get split / trimmed so the new
            // take's edges crossfade against the existing region's audio
            // instead of clicking. Three cases:
            //   - Left overlap  (exStart < newStart, exEnd inside punch):
            //     trim ex to [exStart, newStart + fade], fadeOut at end.
            //   - Right overlap (exStart inside punch, exEnd > newEnd):
            //     trim ex to [newEnd - fade, exEnd] + advance sourceOffset.
            //   - Span (ex wraps both ends): produce two fragments - left
            //     half + right half - sharing the original source file.
            // Fades are matched on the new region by hasOverlapL / R below.
            bool hasOverlapL = false, hasOverlapR = false;
            for (auto it = regs.begin(); it != regs.end(); )
            {
                const auto exStart = it->timelineStart;
                const auto exEnd   = it->timelineStart + it->lengthInSamples;
                const bool overlaps = ! (exEnd <= newStart || exStart >= newEnd);
                if (! overlaps) { ++it; continue; }

                const bool spansLeft  = exStart < newStart;
                const bool spansRight = exEnd   > newEnd;
                const bool containsActive = exStart <= newStart && exEnd >= newEnd;

                if (containsActive)
                {
                    // Keep the overwritten payload as a cycle slot even when
                    // the new partial pass shares one edge with the old take.
                    // Deeper history is compatible only when it covers the
                    // same source-domain slice.
                    const auto sliceOffset = newStart - exStart;
                    const auto sliceLength = newEnd - newStart;
                    TakeRef middle = makeAudioTakeRef (*it);
                    middle.sourceOffset += sliceOffset;
                    middle.lengthInSamples = sliceLength;
                    region.previousTakes.push_back (std::move (middle));
                    for (const auto& deeper : it->previousTakes)
                    {
                        if (deeper.lengthInSamples < sliceOffset + sliceLength)
                            continue;
                        auto sliced = deeper;
                        sliced.sourceOffset += sliceOffset;
                        sliced.lengthInSamples = sliceLength;
                        region.previousTakes.push_back (std::move (sliced));
                    }
                    trimTakeHistory (region);
                }

                if (spansLeft && spansRight)
                {
                    // Span: produce a left fragment + a right fragment from
                    // the same source. Mutate `it` into the left fragment
                    // and queue the right fragment for re-insertion.
                    AudioRegion right = *it;
                    right.timelineStart   = newEnd - fadeSamples;
                    right.sourceOffset    = it->sourceOffset
                                           + (right.timelineStart - it->timelineStart);
                    right.lengthInSamples = exEnd - right.timelineStart;
                    right.fadeInSamples   = fadeSamples;
                    right.fadeInShape     = FadeShape::RaisedCosine;
                    // Right fragment ends at the original exEnd, so any fade-out
                    // the source region carried still applies. Clamp so the new
                    // shorter length still satisfies fadeIn + fadeOut <= length.
                    right.fadeOutSamples  = std::max ((std::int64_t) 0,
                        std::min (right.fadeOutSamples,
                                     right.lengthInSamples - right.fadeInSamples));
                    right.previousTakes.clear();  // history stays with the left half
                    spawnedFragments.push_back (std::move (right));

                    it->lengthInSamples = (newStart + fadeSamples) - exStart;
                    it->fadeOutSamples  = fadeSamples;
                    it->fadeOutShape    = FadeShape::RaisedCosine;
                    hasOverlapL = hasOverlapR = true;
                    ++it;
                }
                else if (spansLeft)
                {
                    // Left overlap only: trim end to newStart + fade.
                    it->lengthInSamples = (newStart + fadeSamples) - exStart;
                    it->fadeOutSamples  = fadeSamples;
                    it->fadeOutShape    = FadeShape::RaisedCosine;
                    hasOverlapL = true;
                    ++it;
                }
                else if (spansRight)
                {
                    // Right overlap only: shift start to newEnd - fade.
                    const std::int64_t newLeft = newEnd - fadeSamples;
                    it->sourceOffset    += (newLeft - exStart);
                    it->timelineStart    = newLeft;
                    it->lengthInSamples  = exEnd - newLeft;
                    it->fadeInSamples    = fadeSamples;
                    it->fadeInShape      = FadeShape::RaisedCosine;
                    hasOverlapR = true;
                    ++it;
                }
                else
                {
                    // Should be unreachable - fully-contained was handled
                    // in Pass 1. Defensive ++ to avoid an infinite loop.
                    ++it;
                }
            }
            for (auto& frag : spawnedFragments)
                regs.push_back (std::move (frag));

            if (hasOverlapL)
            {
                region.fadeInSamples = fadeSamples;
                region.fadeInShape   = FadeShape::RaisedCosine;
            }
            if (hasOverlapR)
            {
                region.fadeOutSamples = fadeSamples;
                region.fadeOutShape   = FadeShape::RaisedCosine;
            }

            regs.push_back (std::move (region));
        }
        else
        {
            if (frames > 0)
            {
                std::fprintf (stderr,
                              "[Dusk Studio/RecordManager] stopRecording: track %d take "
                              "discarded - latency offset head-trim (%lld) consumed all "
                              "%lld recorded frames.\n",
                              t + 1, (long long) trim, (long long) frames);
                lastRecordErrors.push_back (
                    { t, RecordErrorKind::OffsetConsumedTake, (std::uint64_t) frames });
            }
            slot->file.deleteFile();
        }
        slot.reset();
    }

    // Build the per-track diff: only emit an entry for tracks whose
    // regions OR midiRegions actually changed during the commit. The
    // engine reads lastCommitDiff right after this method returns and
    // wraps it in an UndoableAction so Ctrl+Z reverts the take.
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& afterA = session.track (t).regions;
        auto  afterM = session.track (t).midiRegions.current();
        const bool audioChanged = ! (afterA.size() == beforeAudio[(size_t) t].size()
                                      && std::equal (afterA.begin(), afterA.end(),
                                                      beforeAudio[(size_t) t].begin(),
                                                      sameAudioRegion));
        // Deep-compare like the audio path: an overdub that replaces exactly
        // one region keeps the count equal, so size alone misses it and the
        // take becomes un-undoable. Event contents matter too - with
        // previousTakes at its cap, a same-length redo with the same note
        // count would otherwise compare equal.
        const bool midiChanged  = ! (afterM.size() == beforeMidi[(size_t) t].size()
                                      && std::equal (afterM.begin(), afterM.end(),
                                                      beforeMidi[(size_t) t].begin(),
                                                      sameMidiRegion));
        if (! audioChanged && ! midiChanged) continue;

        TrackCommitDiff diff;
        diff.trackIndex  = t;
        diff.audioBefore = std::move (beforeAudio[(size_t) t]);
        diff.audioAfter  = afterA;
        diff.midiBefore  = std::move (beforeMidi[(size_t) t]);
        diff.midiAfter   = std::move (afterM);
        lastCommitDiff.push_back (std::move (diff));
    }
}

void RecordManager::writeMidiBlock (int trackIndex,
                                     const juce::MidiBuffer& events,
                                     std::int64_t blockStartFromRecord) noexcept
{
    AudioInFlightScope guard (audioInFlight);
    if (! active.load (std::memory_order_acquire)) return;
    if (events.isEmpty()) return;
    if (trackIndex < 0 || trackIndex >= Session::kNumTracks) return;
    auto& cap = midiCaptures[(size_t) trackIndex];
    if (cap == nullptr) return;
    writeMidiBlockCalls[(size_t) trackIndex].fetch_add (1, std::memory_order_relaxed);

    for (const auto meta : events)
    {
        const auto m = meta.getMessage();
        const auto* raw = m.getRawData();
        const int   sz  = m.getRawDataSize();
        if (raw == nullptr || sz < 1) continue;

        // Channel-voice messages we care about for 4b: Note On / Note Off
        // / CC / pitch bend / channel pressure / poly pressure / program.
        // System messages (sysex, clock, transport) are intentionally
        // dropped - they're not part of the per-track musical content.
        const auto status = (std::uint8_t) raw[0];
        if (status < 0x80 || status >= 0xF0) continue;
        const auto statusType = status & 0xF0;
        const int expectedSize = (statusType == 0xC0 || statusType == 0xD0)
                               ? 2 : 3;
        if (sz != expectedSize || raw[1] >= 0x80
            || (expectedSize == 3 && raw[2] >= 0x80))
            continue;

        // Loop spans accept the original callback buffer. inputOffset names
        // the first event position in the captured slice; normalize retained
        // events back to pass-relative coordinates before the FIFO write.
        if (loopPlan.enabled
            && (currentLoopSpan.passOrdinal < 1
                || meta.samplePosition < currentLoopSpan.inputOffset
                || meta.samplePosition
                     >= currentLoopSpan.inputOffset + currentLoopSpan.numSamples))
            continue;
        const auto samplePos = loopPlan.enabled
            ? (currentLoopSpan.timelineStart - loopPlan.captureStartSample
               + meta.samplePosition - currentLoopSpan.inputOffset)
            : (blockStartFromRecord + meta.samplePosition);
        if (samplePos < 0) continue;

        int needed = 1;
        if (cap->fifo.getFreeSpace() < needed)
        {
            cap->overflowCount.fetch_add (1, std::memory_order_relaxed);
            // stopRecording cannot consume until active is cleared and this
            // AudioInFlightScope exits, so the producer may safely retire the
            // oldest slot here. Keep the bounded capture biased toward the
            // newest events and loop passes.
            int r1 = 0, rsz1 = 0, r2 = 0, rsz2 = 0;
            cap->fifo.prepareToRead (needed, r1, rsz1, r2, rsz2);
            if (rsz1 + rsz2 < needed)
            {
                cap->fifo.finishedRead (0);
                continue;
            }
            cap->fifo.finishedRead (needed);
        }
        int s1 = 0, sz1 = 0, s2 = 0, sz2 = 0;
        cap->fifo.prepareToWrite (needed, s1, sz1, s2, sz2);
        if (sz1 + sz2 < needed)
        {
            cap->overflowCount.fetch_add (1, std::memory_order_relaxed);
            // Don't advance the write pointer when we didn't write - calling
            // finishedWrite(sz1+sz2) would expose stale/uninitialized slots
            // to the reader on drain. finishedWrite(0) matches the actual
            // bytes written. Rare today (the getFreeSpace guard above
            // usually catches the no-room case first) but not strictly
            // unreachable - getFreeSpace + prepareToWrite aren't atomic,
            // a concurrent drain could shrink the window in between.
            cap->fifo.finishedWrite (0);
            continue;
        }
        auto& slot = cap->events[(size_t) s1];
        slot.samplePos = samplePos;
        slot.status = status;
        slot.data1  = sz >= 2 ? (std::uint8_t) raw[1] : 0;
        slot.data2  = sz >= 3 ? (std::uint8_t) raw[2] : 0;
        slot.passOrdinal = loopPlan.enabled ? currentLoopSpan.passOrdinal : 0;
        cap->fifo.finishedWrite (needed);
    }
}

void RecordManager::writeInputBlock (int trackIndex,
                                     const float* L,
                                     const float* R,
                                     int numSamples) noexcept
{
    AudioInFlightScope guard (audioInFlight);
    if (! active.load (std::memory_order_acquire)) return;
    if (numSamples == 0) return;
    if (trackIndex < 0 || trackIndex >= Session::kNumTracks) return;
    auto& slot = writers[(size_t) trackIndex];
    if (slot == nullptr || slot->writer == nullptr || L == nullptr) return;

    // Build the channel-pointer array to match the writer's channel count.
    // push() reads numChannels pointers from the array, so each slot it
    // touches must be non-null.
    //   - Mono writer (numChannels == 1): only L is read; R is ignored even
    //     if the caller supplied it (mono-armed track + stereo input is a
    //     caller bug, asserted below).
    //   - Stereo writer (numChannels == 2): if R is null we duplicate L so
    //     the second channel is never a missing pointer.
    jassert (L != nullptr);
    if (loopPlan.enabled
        && (currentLoopSpan.passOrdinal < 1 || currentLoopSpan.numSamples <= 0))
        return;
    const int samplesToWrite = loopPlan.enabled
        ? std::min (numSamples, currentLoopSpan.numSamples) : numSamples;
    if (samplesToWrite <= 0) return;

    const float* channels[2] = { L, (R != nullptr) ? R : L };
    jassert (channels[0] != nullptr
             && (slot->numChannels < 2 || channels[1] != nullptr));
    const auto sourceOffset = slot->framesWritten;
    PassDescriptor* descriptor = nullptr;
    if (loopPlan.enabled)
    {
        if (slot->loopPassCount > 0
            && slot->loopPasses[(size_t) (slot->loopPassCount - 1)].passOrdinal
                   == currentLoopSpan.passOrdinal)
        {
            descriptor = &slot->loopPasses[(size_t) (slot->loopPassCount - 1)];
        }
        else
        {
            if (slot->loopPassCount == kRetainedLoopPasses)
            {
                std::move (slot->loopPasses.begin() + 1, slot->loopPasses.end(),
                           slot->loopPasses.begin());
                --slot->loopPassCount;
            }
            descriptor = &slot->loopPasses[(size_t) slot->loopPassCount++];
            *descriptor = {};
            descriptor->passOrdinal = currentLoopSpan.passOrdinal;
            descriptor->timelineStart = loopPlan.captureStartSample;
            descriptor->sourceOffset = sourceOffset;
        }
    }

    if (slot->writer->push (channels, slot->numChannels, samplesToWrite))
    {
        slot->framesWritten += samplesToWrite;
        if (descriptor != nullptr)
        {
            descriptor->lengthInSamples += samplesToWrite;
            descriptor->endsPass = descriptor->endsPass || currentLoopSpan.endsPass;
        }
    }
    else
    {
        slot->writeFailures.fetch_add (1, std::memory_order_relaxed);
        if (descriptor != nullptr)
            descriptor->writeFailed = true;
    }
}
} // namespace duskstudio

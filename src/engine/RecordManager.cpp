#include "RecordManager.h"
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
} // namespace

RecordManager::RecordManager (Session& s) : session (s)
{
    diskThread.startThread();
}

RecordManager::~RecordManager()
{
    if (active.load (std::memory_order_relaxed))
        stopRecording (0);
    diskThread.stopThread (2000);
}

bool RecordManager::startRecording (double sampleRate, juce::int64 startSample)
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

    auto audioDir = session.getAudioDirectory();
    if (! audioDir.exists())
        audioDir.createDirectory();

    recordStartSample = startSample;
    recordSampleRate  = sampleRate;

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
    // recording that captures nothing — fail instead so the caller can surface it.
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
        // wchar_t* and garbles the name into invalid filename characters — the
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

        auto* fileStream = outFile.createOutputStream().release();
        if (fileStream == nullptr)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/RecordManager] startRecording: track %d "
                          "createOutputStream failed for \"%s\" - take will be silently "
                          "dropped without this surface.\n",
                          t + 1, outFile.getFullPathName().toRawUTF8());
            lastSetupFailures.push_back (t);
            continue;
        }

        // 24-bit WAV per the spec. Channel count follows the track's mode:
        // 1 for Mono / Midi (MIDI tracks don't audio-record yet), 2 for
        // Stereo. The writer's channel count is captured here so writeInput-
        // Block builds a matching channel-pointer array on the audio thread.
        const int trackChannels =
            session.track (t).mode.load (std::memory_order_relaxed)
                == (int) Track::Mode::Stereo ? 2 : 1;

        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (fileStream, sampleRate, (unsigned int) trackChannels,
                                  24, {}, 0));
        if (writer == nullptr)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/RecordManager] startRecording: track %d "
                          "createWriterFor failed (format-level error).\n",
                          t + 1);
            lastSetupFailures.push_back (t);
            // juce::AudioFormat::createWriterFor contract: takes ownership
            // of the stream on success AND deletes it on failure. Double-
            // delete here was the prior shape; removed.
            continue;
        }

        // Ring sized to hold ~4 s at the active sample rate so a brief
        // disk stall (NFS hiccup, drive spindown) doesn't push the
        // audio callback into dropped writes. Scaled by sampleRate so a
        // 48k session doesn't pay the 12× memory tax of a 96k worst-
        // case constant: ≈ 1.5 MB / track @ 48k stereo vs ≈ 3 MB @ 96k.
        // Floor at 65536 samples to guarantee a safety margin even on
        // exotic low-rate setups.
        const int kThreadedWriterSamples = juce::jmax (65536, (int) (sampleRate * 4.0));

        // ThreadedWriter ctor allocates its FIFO + worker queue and can
        // throw std::bad_alloc on a memory-starved system. We do NOT catch
        // here: ownership of the raw AudioFormatWriter passes into JUCE
        // mid-ctor and the exact transfer point is implementation-detail,
        // so a catch-and-cleanup is ambiguous (potential double-free if
        // JUCE already destroyed the writer in unwind). On bad_alloc the
        // exception propagates out of startRecording cleanly:
        //   • any per-track writers we already built unwind via the array's
        //     unique_ptr dtors (each drains its FIFO + closes its WAV);
        //   • active was never set to true, so the audio thread sees
        //     active=false and is a no-op;
        //   • caller (AudioEngine::record) sees a false return and surfaces
        //     "recording cannot start" to the user.
        // bad_alloc here means the system is in genuine memory exhaustion;
        // failing the record arm is the correct response.
        auto perTrack = std::make_unique<PerTrackWriter>();
        perTrack->file = outFile;
        perTrack->numChannels = trackChannels;
        perTrack->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
            writer.release(), diskThread, kThreadedWriterSamples);
        if (perTrack->writer == nullptr)
        {
            // Defensive: make_unique returning null is not standard-conformant
            // (it throws on failure, never returns null), but a future custom
            // allocator or instrumented build could. We can't reclaim the raw
            // AudioFormatWriter since its ownership state is undefined here;
            // treat the slot as "armed but inactive" and continue. The
            // existing slot->writer null-check on the audio thread
            // (writeInputBlock) makes the missing entry safe.
            std::fprintf (stderr,
                          "[Dusk Studio/RecordManager] startRecording: track %d "
                          "ThreadedWriter is null after construction; take dropped.\n",
                          t + 1);
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

void RecordManager::stopRecording (juce::int64 endSample)
{
    if (! active.load (std::memory_order_relaxed))
        return;

    active.store (false, std::memory_order_release);

    // Drain in-flight audio-thread calls before reading per-writer
    // counters or destroying writers / midiCaptures. Both writeInputBlock
    // and writeMidiBlock bump audioInFlight before touching their slots;
    // when it reaches zero the audio thread is guaranteed to have left
    // those entry points and any pointers it captured are no longer in
    // use. Yield in a bounded loop — happy-path wait is sub-ms to ~10 ms
    // (one audio block), short enough for the message thread to absorb
    // during a stop transition.
    //
    // Cap at kMaxSpinIterations so a stuck / detached audio thread cannot
    // hang the message thread forever. At a ~1 µs yield cost this is
    // ~1 ms worst-case latency — orders of magnitude over a sane audio
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
    // the slot stays leaked until ~RecordManager — better than a UAF
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
    const juce::int64 totalSamples = juce::jmax ((juce::int64) 1, endSample - recordStartSample);
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
        struct PendingNote { juce::int64 startSample; int velocity; };
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
                n.lengthInTicks = juce::jmax ((juce::int64) 1, offTick - n.startTick);
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
            n.lengthInTicks = juce::jmax ((juce::int64) 1,
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
        const juce::int64 newStart = region.timelineStart;
        const juce::int64 newEnd   = newStart + region.lengthInSamples;
        session.track (t).midiRegions.mutate (
            [&region, newStart, newEnd] (std::vector<MidiRegion>& mregs)
            {
                for (auto it = mregs.begin(); it != mregs.end(); )
                {
                    const auto exStart = it->timelineStart;
                    const auto exEnd   = it->timelineStart + it->lengthInSamples;
                    const bool fullyContained = exStart >= newStart && exEnd <= newEnd;
                    if (! fullyContained) { ++it; continue; }

                    MidiTakeRef ref;
                    ref.lengthInTicks = it->lengthInTicks;
                    ref.notes = std::move (it->notes);
                    ref.ccs   = std::move (it->ccs);
                    region.previousTakes.push_back (std::move (ref));

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
        slot->writer.reset();  // closes the file

        if (frames > 0)
        {
            AudioRegion region;
            region.file = slot->file;
            region.timelineStart = recordStartSample;
            region.lengthInSamples = frames;
            region.sourceOffset = 0;
            region.numChannels = slot->numChannels;

            // Take-history capture: any existing region whose timeline range
            // is FULLY CONTAINED within the new take's range gets absorbed
            // into previousTakes. The user can then cycle through them via
            // the badge UI without losing access to earlier takes.
            //
            // Partial overlaps (e.g. punch-in over the middle of a longer
            // take) are intentionally NOT absorbed - the longer region stays
            // visible on either side of the new take, and the painter just
            // draws the new region on top inside the punch range. Phase 3
            // proper will handle splitting a partially-overlapping region
            // into outer fragments + a new take cycle slot.
            const juce::int64 newStart = region.timelineStart;
            const juce::int64 newEnd   = newStart + region.lengthInSamples;
            auto& regs = session.track (t).regions;

            // Crossfade length: 64 samples per side, raised-cosine
            // shape. DuskStudio.md §5b specifies the click-mask fade as
            // 64 samples ~ 1.3 ms at 48 kHz - imperceptible as a fade
            // but enough to suppress the boundary discontinuity. Bound
            // by half the new take's length so a punch shorter than 128
            // samples still gets symmetric ramps that don't overlap.
            constexpr juce::int64 kPunchFadeSamples = 64;
            const juce::int64 fadeSamples = juce::jmax (
                (juce::int64) 0, juce::jmin (kPunchFadeSamples, region.lengthInSamples / 2));

            // Pass 1 — fully-contained takes get absorbed into the new
            // region's previousTakes (no audio overlap, just history).
            // Partial overlaps fall through to Pass 2 below.
            std::vector<AudioRegion> spawnedFragments;
            for (auto it = regs.begin(); it != regs.end(); )
            {
                const auto exStart = it->timelineStart;
                const auto exEnd   = it->timelineStart + it->lengthInSamples;
                const bool fullyContained = exStart >= newStart && exEnd <= newEnd;
                if (! fullyContained) { ++it; continue; }

                TakeRef ref;
                ref.file            = it->file;
                ref.sourceOffset    = it->sourceOffset;
                ref.lengthInSamples = it->lengthInSamples;
                region.previousTakes.push_back (std::move (ref));

                // Carry forward the displaced region's own history so we
                // don't drop deeper takes when overdubbing repeatedly. The
                // newly-displaced take goes first, then the older ones.
                for (auto& deeper : it->previousTakes)
                    region.previousTakes.push_back (std::move (deeper));

                trimTakeHistory (region);

                it = regs.erase (it);
            }

            // Pass 2 — partial overlaps get split / trimmed so the new
            // take's edges crossfade against the existing region's audio
            // instead of clicking. Three cases:
            //   • Left overlap  (exStart < newStart, exEnd inside punch):
            //     trim ex to [exStart, newStart + fade], fadeOut at end.
            //   • Right overlap (exStart inside punch, exEnd > newEnd):
            //     trim ex to [newEnd - fade, exEnd] + advance sourceOffset.
            //   • Span (ex wraps both ends): produce two fragments — left
            //     half + right half — sharing the original source file.
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
                    right.fadeOutSamples  = juce::jmax ((juce::int64) 0,
                        juce::jmin (right.fadeOutSamples,
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
                    const juce::int64 newLeft = newEnd - fadeSamples;
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
                    // Should be unreachable — fully-contained was handled
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
                                                      [] (const AudioRegion& a, const AudioRegion& b)
                                                      { return a.file == b.file
                                                               && a.timelineStart == b.timelineStart
                                                               && a.lengthInSamples == b.lengthInSamples
                                                               && a.sourceOffset == b.sourceOffset; }));
        // Deep-compare like the audio path: an overdub that replaces exactly
        // one region keeps the count equal, so size alone misses it and the
        // take becomes un-undoable.
        const bool midiChanged  = ! (afterM.size() == beforeMidi[(size_t) t].size()
                                      && std::equal (afterM.begin(), afterM.end(),
                                                      beforeMidi[(size_t) t].begin(),
                                                      [] (const MidiRegion& a, const MidiRegion& b)
                                                      { return a.timelineStart == b.timelineStart
                                                               && a.lengthInTicks == b.lengthInTicks
                                                               && a.notes.size() == b.notes.size()
                                                               && a.ccs.size() == b.ccs.size()
                                                               && a.previousTakes.size() == b.previousTakes.size(); }));
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
                                     juce::int64 blockStartFromRecord) noexcept
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
        const auto status = (juce::uint8) raw[0];
        if (status < 0x80 || status >= 0xF0) continue;

        // Drop events whose absolute take-relative position is negative
        // (count-in pre-roll). They'd be filtered at stopRecording anyway;
        // gating here saves FIFO space and keeps stored samplePos non-negative.
        const auto samplePos = blockStartFromRecord + meta.samplePosition;
        if (samplePos < 0) continue;

        int needed = 1;
        if (cap->fifo.getFreeSpace() < needed)
        {
            cap->overflowCount.fetch_add (1, std::memory_order_relaxed);
            continue;  // FIFO full → drop this event, try next
        }
        int s1 = 0, sz1 = 0, s2 = 0, sz2 = 0;
        cap->fifo.prepareToWrite (needed, s1, sz1, s2, sz2);
        if (sz1 + sz2 < needed)
        {
            cap->overflowCount.fetch_add (1, std::memory_order_relaxed);
            // Don't advance the write pointer when we didn't write — calling
            // finishedWrite(sz1+sz2) would expose stale/uninitialized slots
            // to the reader on drain. finishedWrite(0) matches the actual
            // bytes written. Rare today (the getFreeSpace guard above
            // usually catches the no-room case first) but not strictly
            // unreachable — getFreeSpace + prepareToWrite aren't atomic,
            // a concurrent drain could shrink the window in between.
            cap->fifo.finishedWrite (0);
            continue;
        }
        auto& slot = cap->events[(size_t) s1];
        slot.samplePos = samplePos;
        slot.status = status;
        slot.data1  = sz >= 2 ? (juce::uint8) raw[1] : 0;
        slot.data2  = sz >= 3 ? (juce::uint8) raw[2] : 0;
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
    // ThreadedWriter::write reads exactly numChannels pointers from the
    // array, so each slot it touches must be non-null.
    //   • Mono writer (numChannels == 1): only L is read; R is ignored even
    //     if the caller supplied it (mono-armed track + stereo input is a
    //     caller bug, asserted below).
    //   • Stereo writer (numChannels == 2): if R is null we duplicate L so
    //     the second channel is never a missing pointer.
    jassert (L != nullptr);
    const float* channels[2] = { L, (R != nullptr) ? R : L };
    jassert (channels[0] != nullptr
             && (slot->numChannels < 2 || channels[1] != nullptr));
    if (slot->writer->write (channels, numSamples))
        slot->framesWritten += numSamples;
    else
        slot->writeFailures.fetch_add (1, std::memory_order_relaxed);
}
} // namespace duskstudio

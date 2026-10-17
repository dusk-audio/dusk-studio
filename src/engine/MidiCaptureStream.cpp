#include "MidiCaptureStream.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace duskstudio
{
static_assert (std::atomic<std::uint64_t>::is_always_lock_free,
               "MIDI capture requires lock-free queue positions");
static_assert (std::atomic<int>::is_always_lock_free,
               "MIDI capture requires lock-free pass publication");

MidiCaptureStream::MidiCaptureStream (std::size_t capacity)
    : records (std::max<std::size_t> (1, capacity))
{
    configure ({});
}

void MidiCaptureStream::configure (SampleToTick conversion,
                                  std::int64_t loopCaptureLengthSamples)
{
    sampleToTick = std::move (conversion);
    loopLength = std::max<std::int64_t> (0, loopCaptureLengthSamples);
    linearPreviewLength = -1;
    readPosition.store (0, std::memory_order_relaxed);
    writePosition.store (0, std::memory_order_relaxed);
    attemptedSequence.store (0, std::memory_order_relaxed);
    droppedWrites.store (0, std::memory_order_relaxed);
    completedOrdinal.store (0, std::memory_order_relaxed);
    nextSequence = expectedSequence = 1;
    current = {};
    noteBounds.clear();
    controlSamples.clear();
    heldNotes = {};
    completed.clear();
    lastCompletedOrdinal = 0;
    sealed = false;
    previewNeedsRebuild = true;
    ++generation;
    rebuildPreview();
}

bool MidiCaptureStream::push (Record record) noexcept
{
    // Only a missing payload can invalidate held notes. Progress can be
    // reconstructed from later markers or the final capture bound.
    record.event.sequence = nextSequence;
    if (! record.progress) ++nextSequence;
    const auto write = writePosition.load (std::memory_order_relaxed);
    const auto read = readPosition.load (std::memory_order_acquire);
    const bool fits = write - read < records.size();
    if (fits)
    {
        records[(std::size_t) (write % records.size())] = record;
        writePosition.store (write + 1, std::memory_order_release);
    }
    else if (! record.progress)
    {
        const auto count = droppedWrites.load (std::memory_order_relaxed);
        if (count != std::numeric_limits<std::uint64_t>::max())
            droppedWrites.store (count + 1, std::memory_order_relaxed);
    }
    attemptedSequence.store (nextSequence - 1, std::memory_order_release);
    return fits;
}

bool MidiCaptureStream::pushEvent (int passOrdinal, std::int64_t samplePos,
                                  std::uint8_t status, std::uint8_t data1,
                                  std::uint8_t data2) noexcept
{
    return push ({ { samplePos, passOrdinal, status, data1, data2, 0 }, false, false });
}

bool MidiCaptureStream::publishProgress (int passOrdinal, std::int64_t endSample,
                                        bool complete) noexcept
{
    const bool written = push ({ { endSample, passOrdinal, 0, 0, 0, 0 }, true, complete });
    // A full queue can lose the marker, but the consumer still knows all earlier
    // payload is visible before reconstructing completed loop bounds.
    if (complete && passOrdinal > completedOrdinal.load (std::memory_order_relaxed))
        completedOrdinal.store (passOrdinal, std::memory_order_release);
    return written;
}

std::int64_t MidiCaptureStream::ticksAt (std::int64_t sample) const
{
    sample = std::max<std::int64_t> (0, sample);
    return std::max<std::int64_t> (0, sampleToTick ? sampleToTick (sample) : sample);
}

void MidiCaptureStream::appendHeldNote (int key, std::int64_t endSample,
                                       std::vector<MidiNote>& destination,
                                       bool closeAtOnset,
                                       std::vector<NoteBounds>* bounds) const
{
    const auto& held = heldNotes[(std::size_t) key];
    if (! held.active || held.startSample > endSample
        || (! closeAtOnset && held.startSample == endSample)) return;
    MidiNote note;
    note.channel = key / 128 + 1;
    note.noteNumber = key % 128;
    note.velocity = held.velocity;
    note.startTick = ticksAt (held.startSample);
    if (loopLength > 0)
        note.startTick = std::min (note.startTick, std::max<std::int64_t> (1, ticksAt (loopLength)) - 1);
    note.lengthInTicks = std::max<std::int64_t> (1, ticksAt (endSample) - note.startTick);
    destination.push_back (note);
    if (bounds != nullptr) bounds->push_back ({ held.startSample, endSample });
}

void MidiCaptureStream::recoverGap()
{
    previewNeedsRebuild = true;
    for (int key = 0; key < (int) heldNotes.size(); ++key)
        appendHeldNote (key, current.lengthInSamples, current.notes, false, &noteBounds);
    heldNotes = {};
}

void MidiCaptureStream::finishPass (std::int64_t endSample, bool carryState)
{
    previewNeedsRebuild = true;
    endSample = std::max<std::int64_t> (0, endSample);
    if (loopLength > 0) endSample = std::min (loopLength, endSample);
    current.lengthInSamples = endSample;
    current.lengthInTicks = endSample > 0 ? std::max<std::int64_t> (1, ticksAt (endSample)) : 0;
    for (int key = 0; key < (int) heldNotes.size(); ++key)
        appendHeldNote (key, endSample, current.notes, false, &noteBounds);

    const auto endTick = current.lengthInTicks;
    std::size_t retained = 0;
    for (std::size_t i = 0; i < current.notes.size(); ++i)
    {
        const auto bounds = noteBounds[i];
        if (bounds.start >= endSample) continue;
        auto note = current.notes[i];
        note.startTick = std::min (ticksAt (bounds.start), endTick - 1);
        note.lengthInTicks = std::clamp (ticksAt (std::min (bounds.end, endSample)) - note.startTick,
                                        std::int64_t { 1 }, endTick - note.startTick);
        current.notes[retained++] = note;
    }
    current.notes.resize (retained);
    retained = 0;
    for (std::size_t i = 0; i < current.ccs.size(); ++i)
    {
        if (controlSamples[i] >= endSample) continue;
        auto control = current.ccs[i];
        control.atTick = std::min (ticksAt (controlSamples[i]), endTick - 1);
        current.ccs[retained++] = control;
    }
    current.ccs.resize (retained);
    noteBounds.clear();
    controlSamples.clear();
    std::stable_sort (current.notes.begin(), current.notes.end(),
        [] (const MidiNote& a, const MidiNote& b) { return a.startTick < b.startTick; });
    std::stable_sort (current.ccs.begin(), current.ccs.end(),
        [] (const MidiCc& a, const MidiCc& b) { return a.atTick < b.atTick; });

    const int nextOrdinal = current.passOrdinal == std::numeric_limits<int>::max()
        ? current.passOrdinal : current.passOrdinal + 1;
    lastCompletedOrdinal = current.passOrdinal;
    completed.push_back (std::move (current));
    current = {};
    current.passOrdinal = nextOrdinal;
    if (carryState && loopLength > 0)
    {
        for (auto& held : heldNotes) held.startSample = 0;
    }
    else
    {
        heldNotes = {};
    }
}

void MidiCaptureStream::advanceToPass (int passOrdinal)
{
    while (current.passOrdinal < passOrdinal)
        finishPass (loopLength > 0 ? loopLength : current.lengthInSamples, true);
}

void MidiCaptureStream::consume (const Record& record)
{
    const auto& event = record.event;
    if (event.sequence != expectedSequence) recoverGap();
    expectedSequence = event.sequence + (record.progress ? 0 : 1);
    if (event.passOrdinal < current.passOrdinal || event.passOrdinal < 1
        || event.passOrdinal <= lastCompletedOrdinal) return;
    advanceToPass (event.passOrdinal);

    if (record.progress)
    {
        const auto end = loopLength > 0 ? std::min (loopLength, event.samplePos) : event.samplePos;
        current.lengthInSamples = std::max (current.lengthInSamples, std::max<std::int64_t> (0, end));
        current.lengthInTicks = current.lengthInSamples > 0
            ? std::max<std::int64_t> (1, ticksAt (current.lengthInSamples)) : 0;
        if (record.complete) finishPass (end, true);
        return;
    }

    if (event.samplePos < 0 || (loopLength > 0 && event.samplePos >= loopLength)
        || event.status < 0x80 || event.status >= 0xf0 || event.data1 > 127)
        return;
    const int status = event.status & 0xf0;
    if (status != 0xc0 && status != 0xd0 && event.data2 > 127) return;
    previewNeedsRebuild = true;
    const int key = (event.status & 15) * 128 + event.data1;
    if (status == 0x90 && event.data2 > 0)
    {
        appendHeldNote (key, event.samplePos, current.notes, true, &noteBounds);
        heldNotes[(std::size_t) key] = { true, event.data2, event.samplePos };
    }
    else if (status == 0x80 || status == 0x90)
    {
        appendHeldNote (key, event.samplePos, current.notes, true, &noteBounds);
        heldNotes[(std::size_t) key].active = false;
    }
    else
    {
        const std::uint8_t bytes[] { event.status, event.data1, event.data2 };
        MidiCc control;
        if (MidiCc::fromMessage (bytes, status == 0xc0 || status == 0xd0 ? 2 : 3,
                                 ticksAt (event.samplePos), control))
        {
            if (loopLength > 0)
                control.atTick = std::min (control.atTick, std::max<std::int64_t> (1, ticksAt (loopLength)) - 1);
            current.ccs.push_back (control);
            controlSamples.push_back (event.samplePos);
        }
    }
}

void MidiCaptureStream::rebuildPreview()
{
    constexpr auto noIndex = std::numeric_limits<std::size_t>::max();
    const auto endSample = linearPreviewLength >= 0
        ? std::min (current.lengthInSamples, linearPreviewLength) : current.lengthInSamples;
    const auto endTick = endSample > 0 ? std::max<std::int64_t> (1, ticksAt (endSample)) : 0;
    const auto limitTick = linearPreviewLength >= 0
        ? std::max<std::int64_t> (1, ticksAt (linearPreviewLength)) : 0;
    if (! previewNeedsRebuild)
        for (std::size_t key = 0; key < heldNotes.size(); ++key)
            if (heldNotes[key].active && heldNotes[key].startSample < endSample
                && heldPreviewIndices[key] == noIndex)
            {
                previewNeedsRebuild = true;
                break;
            }
    if (! previewNeedsRebuild)
    {
        previewSnapshot.lengthInSamples = endSample;
        previewSnapshot.lengthInTicks = endTick;
        for (std::size_t key = 0; key < heldNotes.size(); ++key)
            if (heldNotes[key].active && heldPreviewIndices[key] != noIndex)
            {
                auto& note = previewSnapshot.notes[heldPreviewIndices[key]];
                note.lengthInTicks = std::max<std::int64_t> (1, endTick - note.startTick);
            }
        previewSnapshot.generation = generation;
        return;
    }
    static_cast<MidiCapturedPass&> (previewSnapshot) = current;
    previewSnapshot.lengthInSamples = endSample;
    previewSnapshot.lengthInTicks = endTick;
    if (linearPreviewLength >= 0)
    {
        std::size_t retained = 0;
        for (std::size_t i = 0; i < current.notes.size(); ++i)
        {
            const auto bounds = noteBounds[i];
            if (bounds.start >= linearPreviewLength) continue;
            auto note = current.notes[i];
            note.startTick = std::min (ticksAt (bounds.start), limitTick - 1);
            note.lengthInTicks = std::clamp (ticksAt (std::min (bounds.end, linearPreviewLength)) - note.startTick,
                                            std::int64_t { 1 }, limitTick - note.startTick);
            previewSnapshot.notes[retained++] = note;
        }
        previewSnapshot.notes.resize (retained);
        retained = 0;
        for (std::size_t i = 0; i < current.ccs.size(); ++i)
        {
            if (controlSamples[i] >= linearPreviewLength) continue;
            auto control = current.ccs[i];
            control.atTick = std::min (ticksAt (controlSamples[i]), limitTick - 1);
            previewSnapshot.ccs[retained++] = control;
        }
        previewSnapshot.ccs.resize (retained);
    }
    for (int key = 0; key < (int) heldNotes.size(); ++key)
    {
        const auto before = previewSnapshot.notes.size();
        appendHeldNote (key, endSample, previewSnapshot.notes);
        if (linearPreviewLength >= 0 && previewSnapshot.notes.size() != before)
        {
            auto& note = previewSnapshot.notes.back();
            note.startTick = std::min (note.startTick, limitTick - 1);
            note.lengthInTicks = std::max<std::int64_t> (1, endTick - note.startTick);
        }
    }
    std::stable_sort (previewSnapshot.notes.begin(), previewSnapshot.notes.end(),
        [] (const MidiNote& a, const MidiNote& b) { return a.startTick < b.startTick; });
    heldPreviewIndices.fill (noIndex);
    for (std::size_t i = 0; i < previewSnapshot.notes.size(); ++i)
    {
        const auto& note = previewSnapshot.notes[i];
        const auto key = (std::size_t) ((note.channel - 1) * 128 + note.noteNumber);
        const auto& held = heldNotes[key];
        auto heldStartTick = ticksAt (held.startSample);
        if (loopLength > 0)
            heldStartTick = std::min (heldStartTick, std::max<std::int64_t> (1, ticksAt (loopLength)) - 1);
        if (linearPreviewLength >= 0)
            heldStartTick = std::min (heldStartTick, limitTick - 1);
        // Stable sorting leaves the appended held note after equal closed
        // notes, so the last matching entry is the one whose length can grow.
        if (held.active && held.startSample < endSample
            && note.velocity == held.velocity && note.startTick == heldStartTick)
            heldPreviewIndices[key] = i;
    }
    previewNeedsRebuild = false;
    previewSnapshot.generation = generation;
}

bool MidiCaptureStream::service (std::int64_t linearCaptureLength)
{
    if (sealed) return false;
    const bool limitChanged = linearPreviewLength != linearCaptureLength;
    linearPreviewLength = linearCaptureLength;
    previewNeedsRebuild = previewNeedsRebuild || limitChanged;
    // This order guarantees the ready snapshot contains every retained payload
    // preceding the acquired completion, even when its marker overflowed.
    const int completedSnapshot = completedOrdinal.load (std::memory_order_acquire);
    const auto attemptedSnapshot = attemptedSequence.load (std::memory_order_acquire);
    const auto readyEnd = writePosition.load (std::memory_order_acquire);
    auto read = readPosition.load (std::memory_order_relaxed);
    bool changed = limitChanged || read != readyEnd;
    while (read != readyEnd)
    {
        consume (records[(std::size_t) (read % records.size())]);
        ++read;
        readPosition.store (read, std::memory_order_release);
    }
    if (attemptedSnapshot >= expectedSequence)
    {
        recoverGap();
        expectedSequence = attemptedSnapshot + 1;
        changed = true;
    }
    if (loopLength > 0)
        while (current.passOrdinal <= completedSnapshot
               && lastCompletedOrdinal < completedSnapshot)
        {
            finishPass (loopLength, true);
            changed = true;
            if (current.passOrdinal == std::numeric_limits<int>::max()) break;
        }
    if (changed)
    {
        ++generation;
        rebuildPreview();
    }
    return changed;
}

void MidiCaptureStream::sealFinal (int passOrdinal, std::int64_t endSample)
{
    if (sealed) return;
    service();
    if (passOrdinal >= current.passOrdinal && passOrdinal >= 1
        && passOrdinal > lastCompletedOrdinal)
    {
        advanceToPass (passOrdinal);
        finishPass (endSample, false);
    }
    heldNotes = {};
    current = {};
    current.passOrdinal = std::max (1, passOrdinal);
    sealed = true;
    previewNeedsRebuild = true;
    ++generation;
    rebuildPreview();
}

std::vector<MidiCapturedPass> MidiCaptureStream::takeCompleted()
{
    auto result = std::move (completed);
    completed.clear();
    return result;
}
} // namespace duskstudio

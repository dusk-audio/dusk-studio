#include <catch2/catch_test_macros.hpp>

#include "engine/RecordManager.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace duskstudio;

namespace
{
struct ScopedDir
{
    juce::File dir;
    ~ScopedDir() { dir.deleteRecursively(); }
};

ScopedDir makeSessionDir (const char* tag)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile (juce::String (tag)
                                  + juce::String (juce::Random::getSystemRandom().nextInt()));
    REQUIRE (dir.createDirectory().wasOk());
    return { dir };
}

RecordManager::LoopCapturePlan loopPlan (std::int64_t start = 100,
                                         std::int64_t end = 108)
{
    RecordManager::LoopCapturePlan plan;
    plan.enabled = true;
    plan.loopStartSample = start;
    plan.loopEndSample = end;
    plan.captureStartSample = start;
    plan.captureEndSample = end;
    return plan;
}

void armTrack (Session& session, const juce::File& dir, Track::Mode mode)
{
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) mode, std::memory_order_relaxed);
    session.setTrackArmed (0, true);
}

void writeAudioPass (RecordManager& manager,
                     int ordinal,
                     std::int64_t timelineStart,
                     int samples,
                     float value = 0.25f)
{
    const auto span = manager.coordinateLoopCaptureSpan (
        ordinal, timelineStart, 0, samples);
    REQUIRE (span.passOrdinal == ordinal);
    REQUIRE (span.numSamples == samples);
    manager.beginLoopCaptureSpan (span);
    std::vector<float> block ((size_t) samples, value);
    manager.writeInputBlock (0, block.data() + span.inputOffset, nullptr, span.numSamples);
}

void writeMidiPass (RecordManager& manager,
                    int ordinal,
                    std::int64_t timelineStart,
                    juce::MidiBuffer events,
                    int samples = 8)
{
    const auto span = manager.coordinateLoopCaptureSpan (
        ordinal, timelineStart, 0, samples);
    REQUIRE (span.passOrdinal == ordinal);
    REQUIRE (span.numSamples == samples);
    manager.beginLoopCaptureSpan (span);
    manager.writeMidiBlock (0, events, 0);
}

const AudioRegion& loopAudioRegion (const Session& session)
{
    const auto& regions = session.track (0).regions;
    const auto it = std::find_if (regions.begin(), regions.end(), [] (const AudioRegion& region)
    {
        return region.provenance.loopPassOrdinal > 0;
    });
    REQUIRE (it != regions.end());
    return *it;
}

juce::MidiBuffer oneNote (int note, int onSample = 1, int offSample = 6)
{
    juce::MidiBuffer events;
    events.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), onSample);
    events.addEvent (juce::MidiMessage::noteOff (1, note), offSample);
    return events;
}
} // namespace

TEST_CASE ("Loop audio stores two full passes as one spool with exact take offsets",
           "[recording][recordmanager][loop-takes]")
{
    const auto temp = makeSessionDir ("dusk-loop-audio-two-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);
    const auto plan = loopPlan();

    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));
    writeAudioPass (manager, 1, 100, 8);
    writeAudioPass (manager, 2, 100, 8);
    manager.stopRecording (108);

    const auto& region = loopAudioRegion (session);
    REQUIRE (region.timelineStart == 100);
    REQUIRE (region.sourceOffset == 8);
    REQUIRE (region.lengthInSamples == 8);
    REQUIRE (region.provenance.loopPassOrdinal == 2);
    REQUIRE_FALSE (region.provenance.partialPass);
    REQUIRE (region.provenance.capturedAtMs > 0);
    REQUIRE (region.previousTakes.size() == 1);
    REQUIRE (region.previousTakes[0].file == region.file);
    REQUIRE (region.previousTakes[0].sourceOffset == 0);
    REQUIRE (region.previousTakes[0].lengthInSamples == 8);
    REQUIRE (region.previousTakes[0].provenance.loopPassOrdinal == 1);
    REQUIRE (region.previousTakes[0].provenance.capturedAtMs
             == region.provenance.capturedAtMs);
}

TEST_CASE ("Loop audio commits a final partial pass and no exact-boundary empty pass",
           "[recording][recordmanager][loop-takes]")
{
    const auto temp = makeSessionDir ("dusk-loop-audio-partial-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);
    const auto plan = loopPlan();

    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));
    writeAudioPass (manager, 1, 100, 8);

    SECTION ("final partial")
    {
        writeAudioPass (manager, 2, 100, 3);
        manager.stopRecording (103);
        const auto& region = loopAudioRegion (session);
        REQUIRE (region.provenance.loopPassOrdinal == 2);
        REQUIRE (region.provenance.partialPass);
        REQUIRE (region.sourceOffset == 8);
        REQUIRE (region.lengthInSamples == 3);
        REQUIRE (region.previousTakes.size() == 1);
    }

    SECTION ("exact loop boundary")
    {
        writeAudioPass (manager, 2, 100, 8);
        manager.stopRecording (108);
        const auto& region = loopAudioRegion (session);
        REQUIRE (region.provenance.loopPassOrdinal == 2);
        REQUIRE_FALSE (region.provenance.partialPass);
        REQUIRE (region.previousTakes.size() == 1);
    }
}

TEST_CASE ("Loop audio excludes a pass after a failed writer push without compressing it",
           "[recording][recordmanager][loop-takes][failure]")
{
    const auto temp = makeSessionDir ("dusk-loop-audio-failed-pass-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);
    const auto plan = loopPlan (0, 65538);

    REQUIRE (manager.startRecording (1.0, 0, 0, plan));
    writeAudioPass (manager, 1, 0, 65537);
    writeAudioPass (manager, 1, 65537, 1);
    writeAudioPass (manager, 2, 0, 4);
    manager.stopRecording (4);

    const auto& region = loopAudioRegion (session);
    REQUIRE (region.provenance.loopPassOrdinal == 2);
    REQUIRE (region.sourceOffset == 1);
    REQUIRE (region.lengthInSamples == 4);
    REQUIRE (region.previousTakes.empty());

    const auto& errors = manager.getLastRecordErrors();
    REQUIRE (errors.size() == 1);
    REQUIRE (errors[0].kind == RecordManager::RecordErrorKind::WavWrite);
    REQUIRE (errors[0].count == 1);
}

TEST_CASE ("Loop audio history is newest-first capped and then retains compatible old takes",
           "[recording][recordmanager][loop-takes][history]")
{
    const auto temp = makeSessionDir ("dusk-loop-audio-history-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);

    AudioRegion existing;
    existing.file = temp.dir.getChildFile ("existing.wav");
    existing.timelineStart = 100;
    existing.lengthInSamples = 8;
    existing.sourceOffset = 40;
    existing.provenance.capturedAtMs = 77;
    existing.previousTakes.push_back (
        { temp.dir.getChildFile ("existing-older.wav"), 20, 8, { 66, 0, false } });
    session.track (0).regions.push_back (existing);

    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));
    for (int ordinal = 1; ordinal <= 7; ++ordinal)
        writeAudioPass (manager, ordinal, 100, 8);
    manager.stopRecording (108);

    const auto& region = loopAudioRegion (session);
    REQUIRE (region.provenance.loopPassOrdinal == 7);
    REQUIRE (region.previousTakes.size() == 8);
    for (int i = 0; i < 6; ++i)
        REQUIRE (region.previousTakes[(size_t) i].provenance.loopPassOrdinal == 6 - i);
    REQUIRE (region.previousTakes[6].file == existing.file);
    REQUIRE (region.previousTakes[6].provenance.capturedAtMs == 77);
    REQUIRE (region.previousTakes[7].file == existing.previousTakes[0].file);
}

TEST_CASE ("Loop audio retention drops oldest passes after current plus eight prior",
           "[recording][recordmanager][loop-takes][history]")
{
    const auto temp = makeSessionDir ("dusk-loop-audio-cap-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));
    for (int ordinal = 1; ordinal <= 10; ++ordinal)
        writeAudioPass (manager, ordinal, 100, 8);
    manager.stopRecording (108);

    const auto& region = loopAudioRegion (session);
    REQUIRE (region.provenance.loopPassOrdinal == 10);
    REQUIRE (region.previousTakes.size() == 8);
    for (int i = 0; i < 8; ++i)
        REQUIRE (region.previousTakes[(size_t) i].provenance.loopPassOrdinal == 9 - i);
}

TEST_CASE ("Loop audio retains a silent pass and trims latency per pass",
           "[recording][recordmanager][loop-takes][latency]")
{
    const auto temp = makeSessionDir ("dusk-loop-audio-latency-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);
    const auto plan = loopPlan (0, 8);
    REQUIRE (manager.startRecording (48000.0, 0, 3, plan));
    writeAudioPass (manager, 1, 0, 8, 0.0f);
    writeAudioPass (manager, 2, 0, 8, 0.5f);
    manager.stopRecording (8);

    const auto& region = loopAudioRegion (session);
    REQUIRE (region.timelineStart == 0);
    REQUIRE (region.sourceOffset == 11);
    REQUIRE (region.lengthInSamples == 5);
    REQUIRE (region.previousTakes.size() == 1);
    REQUIRE (region.previousTakes[0].sourceOffset == 3);
    REQUIRE (region.previousTakes[0].lengthInSamples == 5);
}

TEST_CASE ("Loop punch preserves the overwritten middle of a spanning region as a take",
           "[recording][recordmanager][loop-takes][punch]")
{
    const auto temp = makeSessionDir ("dusk-loop-audio-span-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);

    AudioRegion existing;
    existing.file = temp.dir.getChildFile ("spanning.wav");
    existing.timelineStart = 90;
    existing.lengthInSamples = 30;
    existing.sourceOffset = 50;
    existing.provenance = { 42, 0, false };
    existing.previousTakes.push_back (
        { temp.dir.getChildFile ("spanning-older.wav"), 10, 30, { 41, 0, false } });
    existing.previousTakes.push_back (
        { temp.dir.getChildFile ("spanning-too-short.wav"), 5, 12, { 40, 0, false } });
    session.track (0).regions.push_back (existing);

    RecordManager manager (session);
    const auto plan = loopPlan (100, 110);
    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));
    writeAudioPass (manager, 1, 100, 10);
    manager.stopRecording (110);

    const auto& region = loopAudioRegion (session);
    REQUIRE (region.previousTakes.size() == 2);
    REQUIRE (region.previousTakes[0].file == existing.file);
    REQUIRE (region.previousTakes[0].sourceOffset == 60);
    REQUIRE (region.previousTakes[0].lengthInSamples == 10);
    REQUIRE (region.previousTakes[0].provenance.capturedAtMs == 42);
    REQUIRE (region.previousTakes[1].file == existing.previousTakes[0].file);
    REQUIRE (region.previousTakes[1].sourceOffset == 20);
    REQUIRE (region.previousTakes[1].lengthInSamples == 10);
    REQUIRE (std::none_of (region.previousTakes.begin(), region.previousTakes.end(),
                           [&existing] (const TakeRef& take)
    {
        return take.file == existing.previousTakes[1].file;
    }));
    REQUIRE (session.track (0).regions.size() == 3);
}

TEST_CASE ("Final partial audio keeps loop history before sliced containing takes",
           "[recording][recordmanager][loop-takes][history][punch]")
{
    const auto temp = makeSessionDir ("dusk-loop-audio-partial-history-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);

    AudioRegion existing;
    existing.file = temp.dir.getChildFile ("existing-full.wav");
    existing.timelineStart = 100;
    existing.lengthInSamples = 8;
    existing.sourceOffset = 40;
    existing.provenance = { 81, 0, false };
    existing.previousTakes.push_back (
        { temp.dir.getChildFile ("existing-full-older.wav"), 20, 8,
          { 80, 0, false } });
    session.track (0).regions.push_back (existing);

    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));
    writeAudioPass (manager, 1, 100, 8);
    writeAudioPass (manager, 2, 100, 5);
    manager.stopRecording (105);

    const auto& region = loopAudioRegion (session);
    REQUIRE (region.provenance.loopPassOrdinal == 2);
    REQUIRE (region.provenance.partialPass);
    REQUIRE (region.previousTakes.size() == 3);
    REQUIRE (region.previousTakes[0].provenance.loopPassOrdinal == 1);
    REQUIRE (region.previousTakes[1].file == existing.file);
    REQUIRE (region.previousTakes[1].sourceOffset == 40);
    REQUIRE (region.previousTakes[1].lengthInSamples == 5);
    REQUIRE (region.previousTakes[1].provenance.capturedAtMs == 81);
    REQUIRE (region.previousTakes[2].file == existing.previousTakes[0].file);
    REQUIRE (region.previousTakes[2].sourceOffset == 20);
    REQUIRE (region.previousTakes[2].lengthInSamples == 5);

    const auto& regions = session.track (0).regions;
    const auto tail = std::find_if (regions.begin(), regions.end(),
                                    [&existing] (const AudioRegion& candidate)
    {
        return candidate.file == existing.file
            && candidate.provenance.loopPassOrdinal == 0;
    });
    REQUIRE (tail != regions.end());
    REQUIRE (tail->timelineStart < 108);
    REQUIRE (tail->timelineStart + tail->lengthInSamples == 108);
}

TEST_CASE ("Loop MIDI partitions two passes into current and previous takes",
           "[recording][recordmanager][loop-takes][midi]")
{
    const auto temp = makeSessionDir ("dusk-loop-midi-two-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Midi);
    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (960.0, 100, 0, plan));
    writeMidiPass (manager, 1, 100, oneNote (60), 8);
    writeMidiPass (manager, 2, 100, oneNote (64), 8);
    manager.stopRecording (108);

    const auto regions = session.track (0).midiRegions.current();
    REQUIRE (regions.size() == 1);
    const auto& region = regions[0];
    REQUIRE (region.provenance.loopPassOrdinal == 2);
    REQUIRE (region.notes.size() == 1);
    REQUIRE (region.notes[0].noteNumber == 64);
    REQUIRE (region.previousTakes.size() == 1);
    REQUIRE (region.previousTakes[0].provenance.loopPassOrdinal == 1);
    REQUIRE (region.previousTakes[0].notes.size() == 1);
    REQUIRE (region.previousTakes[0].notes[0].noteNumber == 60);
}

TEST_CASE ("Loop MIDI history keeps loop passes before compatible existing takes",
           "[recording][recordmanager][loop-takes][midi][history]")
{
    const auto temp = makeSessionDir ("dusk-loop-midi-history-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Midi);

    MidiRegion existing;
    existing.timelineStart = 100;
    existing.lengthInSamples = 8;
    existing.lengthInTicks = 8;
    existing.provenance = { 70, 0, false };
    existing.notes.push_back ({ 1, 50, 90, 1, 4 });
    MidiTakeRef deeper;
    deeper.lengthInTicks = 8;
    deeper.provenance = { 60, 0, false };
    existing.previousTakes.push_back (deeper);
    session.track (0).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (1, existing));

    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (960.0, 100, 0, plan));
    for (int ordinal = 1; ordinal <= 7; ++ordinal)
        writeMidiPass (manager, ordinal, 100, oneNote (59 + ordinal), 8);
    manager.stopRecording (108);

    const auto region = session.track (0).midiRegions.current().at (0);
    REQUIRE (region.provenance.loopPassOrdinal == 7);
    REQUIRE (region.previousTakes.size() == 8);
    for (int i = 0; i < 6; ++i)
        REQUIRE (region.previousTakes[(size_t) i].provenance.loopPassOrdinal == 6 - i);
    REQUIRE (region.previousTakes[6].provenance.capturedAtMs == 70);
    REQUIRE (region.previousTakes[7].provenance.capturedAtMs == 60);
}

TEST_CASE ("Loop capture freezes the successfully armed track set for the gesture",
           "[recording][recordmanager][loop-takes][arming]")
{
    const auto temp = makeSessionDir ("dusk-loop-armed-snapshot-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    session.track (1).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    session.setTrackArmed (1, true);
    session.track (2).mode.store ((int) Track::Mode::Stereo, std::memory_order_relaxed);
    session.setTrackArmed (2, true);

    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));
    REQUIRE (manager.getActiveCaptureTrackMask() == 0x7u);
    REQUIRE (manager.getActiveMidiCaptureTrackMask() == 0x2u);
    REQUIRE (manager.getActiveStereoCaptureTrackMask() == 0x4u);

    session.setTrackArmed (0, false);
    session.setTrackArmed (1, false);
    session.setTrackArmed (2, false);
    session.setTrackArmed (3, true);
    session.track (0).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    session.track (1).mode.store ((int) Track::Mode::Stereo, std::memory_order_relaxed);
    session.track (2).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    REQUIRE (manager.getActiveCaptureTrackMask() == 0x7u);
    REQUIRE (manager.getActiveMidiCaptureTrackMask() == 0x2u);
    REQUIRE (manager.getActiveStereoCaptureTrackMask() == 0x4u);

    manager.stopRecording (100);
    REQUIRE (manager.getActiveCaptureTrackMask() == 0u);
    REQUIRE (manager.getActiveMidiCaptureTrackMask() == 0u);
    REQUIRE (manager.getActiveStereoCaptureTrackMask() == 0u);
}

TEST_CASE ("Loop MIDI finalization is bounded to the retained high-ordinal passes",
           "[recording][recordmanager][loop-takes][midi][history]")
{
    const auto temp = makeSessionDir ("dusk-loop-midi-high-ordinal-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Midi);
    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (960.0, 100, 0, plan));

    constexpr int firstOrdinal = 99992;
    constexpr int lastOrdinal = 100000;
    for (int ordinal = firstOrdinal; ordinal <= lastOrdinal; ++ordinal)
        writeMidiPass (manager, ordinal, 100,
                       oneNote (60 + ordinal - firstOrdinal), 8);
    manager.stopRecording (108);

    const auto region = session.track (0).midiRegions.current().at (0);
    REQUIRE (region.provenance.loopPassOrdinal == lastOrdinal);
    REQUIRE (region.previousTakes.size() == 8);
    for (int i = 0; i < 8; ++i)
        REQUIRE (region.previousTakes[(size_t) i].provenance.loopPassOrdinal
                 == lastOrdinal - 1 - i);
}

TEST_CASE ("Loop MIDI closes and retriggers a note crossing the seam",
           "[recording][recordmanager][loop-takes][midi]")
{
    const auto temp = makeSessionDir ("dusk-loop-midi-note-seam-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Midi);
    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (960.0, 100, 0, plan));

    juce::MidiBuffer first;
    first.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 91), 6);
    writeMidiPass (manager, 1, 100, first, 8);
    juce::MidiBuffer second;
    second.addEvent (juce::MidiMessage::noteOff (1, 67), 2);
    writeMidiPass (manager, 2, 100, second, 8);
    manager.stopRecording (108);

    const auto region = session.track (0).midiRegions.current().at (0);
    REQUIRE (region.previousTakes.size() == 1);
    REQUIRE (region.previousTakes[0].notes.size() == 1);
    REQUIRE (region.previousTakes[0].notes[0].startTick == 6);
    REQUIRE (region.previousTakes[0].notes[0].lengthInTicks == 2);
    REQUIRE (region.notes.size() == 1);
    REQUIRE (region.notes[0].startTick == 0);
    REQUIRE (region.notes[0].lengthInTicks == 2);
    REQUIRE (region.notes[0].velocity == 91);
}

TEST_CASE ("Loop MIDI resets and chases sustain across a seam",
           "[recording][recordmanager][loop-takes][midi]")
{
    const auto temp = makeSessionDir ("dusk-loop-midi-cc-seam-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Midi);
    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (960.0, 100, 0, plan));

    juce::MidiBuffer first;
    first.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 2);
    writeMidiPass (manager, 1, 100, first, 8);
    juce::MidiBuffer second;
    second.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 3);
    writeMidiPass (manager, 2, 100, second, 8);
    manager.stopRecording (108);

    const auto region = session.track (0).midiRegions.current().at (0);
    REQUIRE (region.previousTakes.size() == 1);
    const auto& priorCcs = region.previousTakes[0].ccs;
    REQUIRE (priorCcs.size() == 2);
    REQUIRE (priorCcs[0].controller == 64);
    REQUIRE (priorCcs[0].value == 127);
    REQUIRE (priorCcs[1].value == 0);
    REQUIRE (priorCcs[1].atTick == 7);
    REQUIRE (priorCcs[1].atTick < region.previousTakes[0].lengthInTicks);
    REQUIRE (region.ccs.size() == 2);
    REQUIRE (region.ccs[0].controller == 64);
    REQUIRE (region.ccs[0].value == 127);
    REQUIRE (region.ccs[0].atTick == 0);
    REQUIRE (region.ccs[1].value == 0);
    REQUIRE (region.ccs[1].atTick == 3);
}

TEST_CASE ("Loop MIDI rejects malformed channel message sizes and data bytes",
           "[recording][recordmanager][loop-takes][midi][malformed]")
{
    const auto temp = makeSessionDir ("dusk-loop-midi-malformed-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Midi);
    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (960.0, 100, 0, plan));

    const std::uint8_t invalidNoteData[] = { 0x90, 62, 0x80 };
    const std::uint8_t invalidCcData[] = { 0xB0, 0xFF, 127 };
    const std::uint8_t truncatedNote[] = { 0x90, 64 };
    juce::MidiBuffer events;
    events.addEvent (invalidNoteData, 3, 1);
    events.addEvent (invalidCcData, 3, 2);
    events.addEvent (juce::MidiMessage::noteOn (
                         1, 64, (juce::uint8) 100), 4);
    events.addEvent (truncatedNote, 2, 5);
    events.addEvent (juce::MidiMessage::noteOff (1, 64), 6);
    writeMidiPass (manager, 1, 100, std::move (events), 8);
    manager.stopRecording (108);

    const auto region = session.track (0).midiRegions.current().at (0);
    REQUIRE (region.notes.size() == 1);
    REQUIRE (region.notes[0].noteNumber == 64);
    REQUIRE (region.notes[0].lengthInTicks == 2);
    REQUIRE (region.ccs.empty());
}

TEST_CASE ("Loop MIDI overflow retains the newest pass and latches the loss",
           "[recording][recordmanager][loop-takes][midi][overflow]")
{
    constexpr int capacity = 65536;
    const auto temp = makeSessionDir ("dusk-loop-midi-newest-overflow-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Midi);
    RecordManager manager (session);
    const auto plan = loopPlan (0, capacity);
    REQUIRE (manager.startRecording (960.0, 0, 0, plan));

    juce::MidiBuffer oldPass;
    for (int sample = 0; sample < capacity; ++sample)
        oldPass.addEvent (juce::MidiMessage::controllerEvent (1, 1, 1), sample);
    writeMidiPass (manager, 1, 0, std::move (oldPass), capacity);
    writeMidiPass (manager, 2, 0, oneNote (72, 0, 2), 4);
    manager.stopRecording (4);

    const auto region = session.track (0).midiRegions.current().at (0);
    REQUIRE (region.provenance.loopPassOrdinal == 2);
    REQUIRE (region.notes.size() == 1);
    REQUIRE (region.notes[0].noteNumber == 72);
    REQUIRE (region.previousTakes.size() == 1);
    REQUIRE (region.previousTakes[0].provenance.loopPassOrdinal == 1);

    const auto& errors = manager.getLastRecordErrors();
    REQUIRE (errors.size() == 1);
    REQUIRE (errors[0].kind == RecordManager::RecordErrorKind::MidiOverflow);
    REQUIRE (errors[0].count == 2);
}

TEST_CASE ("Loop MIDI ignores a truly empty pass and keeps a final partial pass",
           "[recording][recordmanager][loop-takes][midi]")
{
    const auto temp = makeSessionDir ("dusk-loop-midi-empty-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Midi);
    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (960.0, 100, 0, plan));
    writeMidiPass (manager, 1, 100, {}, 8);
    writeMidiPass (manager, 2, 100, oneNote (72, 0, 2), 3);
    manager.stopRecording (103);

    const auto region = session.track (0).midiRegions.current().at (0);
    REQUIRE (region.provenance.loopPassOrdinal == 2);
    REQUIRE (region.provenance.partialPass);
    REQUIRE (region.previousTakes.empty());
    REQUIRE (region.lengthInSamples == 3);
}

TEST_CASE ("Loop MIDI accepts original punch callback coordinates",
           "[recording][recordmanager][loop-takes][midi][punch]")
{
    const auto temp = makeSessionDir ("dusk-loop-midi-punch-coordinates-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Midi);
    RecordManager manager (session);
    auto plan = loopPlan (100, 120);
    plan.captureStartSample = 105;
    plan.captureEndSample = 108;
    REQUIRE (manager.startRecording (960.0, 105, 0, plan));

    const auto span = manager.coordinateLoopCaptureSpan (1, 100, 0, 12);
    REQUIRE (span.inputOffset == 5);
    REQUIRE (span.numSamples == 3);
    manager.beginLoopCaptureSpan (span);

    juce::MidiBuffer originalCallback;
    originalCallback.addEvent (juce::MidiMessage::noteOn (
                                   1, 50, (juce::uint8) 100), 1);
    originalCallback.addEvent (juce::MidiMessage::noteOff (1, 50), 3);
    originalCallback.addEvent (juce::MidiMessage::noteOn (
                                   1, 60, (juce::uint8) 100), 5);
    originalCallback.addEvent (juce::MidiMessage::noteOff (1, 60), 7);
    originalCallback.addEvent (juce::MidiMessage::noteOn (
                                   1, 70, (juce::uint8) 100), 8);
    originalCallback.addEvent (juce::MidiMessage::noteOff (1, 70), 10);
    manager.writeMidiBlock (0, originalCallback, 0);
    manager.stopRecording (108);

    const auto region = session.track (0).midiRegions.current().at (0);
    REQUIRE (region.notes.size() == 1);
    REQUIRE (region.notes[0].noteNumber == 60);
    REQUIRE (region.notes[0].startTick == 0);
    REQUIRE (region.notes[0].lengthInTicks == 2);
}

TEST_CASE ("Loop MIDI splits one original callback across a seam without rereading events",
           "[recording][recordmanager][loop-takes][midi][seam]")
{
    const auto temp = makeSessionDir ("dusk-loop-midi-one-callback-seam-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Midi);
    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (960.0, 100, 0, plan));

    // The first six samples of pass 1 arrived in an earlier callback. This
    // seam callback begins at timeline 106 and wraps after its first 2 samples.
    auto span = manager.coordinateLoopCaptureSpan (1, 100, 0, 6);
    REQUIRE (span.inputOffset == 0);
    REQUIRE (span.numSamples == 6);
    manager.beginLoopCaptureSpan (span);

    juce::MidiBuffer originalCallback;
    originalCallback.addEvent (juce::MidiMessage::noteOn (
                                   1, 60, (juce::uint8) 100), 0);
    originalCallback.addEvent (juce::MidiMessage::noteOff (1, 60), 1);
    originalCallback.addEvent (juce::MidiMessage::noteOn (
                                   1, 64, (juce::uint8) 100), 4);
    originalCallback.addEvent (juce::MidiMessage::noteOff (1, 64), 5);

    span = manager.coordinateLoopCaptureSpan (1, 106, 0, 6);
    REQUIRE (span.inputOffset == 0);
    REQUIRE (span.numSamples == 2);
    REQUIRE (span.endsPass);
    manager.beginLoopCaptureSpan (span);
    manager.writeMidiBlock (0, originalCallback, 0);

    span = manager.coordinateLoopCaptureSpan (2, 100, 2, 4);
    REQUIRE (span.inputOffset == 2);
    REQUIRE (span.numSamples == 4);
    manager.beginLoopCaptureSpan (span);
    manager.writeMidiBlock (0, originalCallback, 0);
    manager.stopRecording (104);

    const auto region = session.track (0).midiRegions.current().at (0);
    REQUIRE (region.provenance.loopPassOrdinal == 2);
    REQUIRE (region.notes.size() == 1);
    REQUIRE (region.notes[0].noteNumber == 64);
    REQUIRE (region.notes[0].startTick == 2);
    REQUIRE (region.notes[0].lengthInTicks == 1);
    REQUIRE (region.previousTakes.size() == 1);
    REQUIRE (region.previousTakes[0].provenance.loopPassOrdinal == 1);
    REQUIRE (region.previousTakes[0].notes.size() == 1);
    REQUIRE (region.previousTakes[0].notes[0].noteNumber == 60);
    REQUIRE (region.previousTakes[0].notes[0].startTick == 6);
    REQUIRE (region.previousTakes[0].notes[0].lengthInTicks == 1);
}

TEST_CASE ("Final partial MIDI keeps containing history after loop passes without removal",
           "[recording][recordmanager][loop-takes][midi][history][punch]")
{
    const auto temp = makeSessionDir ("dusk-loop-midi-partial-history-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Midi);

    MidiRegion existing;
    existing.timelineStart = 100;
    existing.lengthInSamples = 8;
    existing.lengthInTicks = 8;
    existing.provenance = { 91, 0, false };
    existing.notes.push_back ({ 1, 48, 90, 0, 6 });
    MidiTakeRef deeper;
    deeper.lengthInTicks = 8;
    deeper.notes.push_back ({ 1, 47, 80, 0, 7 });
    deeper.provenance = { 90, 0, false };
    existing.previousTakes.push_back (deeper);
    session.track (0).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (1, existing));

    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (960.0, 100, 0, plan));
    writeMidiPass (manager, 1, 100, oneNote (60), 8);
    writeMidiPass (manager, 2, 100, oneNote (64, 0, 2), 3);
    manager.stopRecording (103);

    const auto regions = session.track (0).midiRegions.current();
    REQUIRE (regions.size() == 2);
    const auto current = std::find_if (regions.begin(), regions.end(), [] (const MidiRegion& r)
    {
        return r.provenance.loopPassOrdinal == 2;
    });
    REQUIRE (current != regions.end());
    REQUIRE (current->previousTakes.size() == 3);
    REQUIRE (current->previousTakes[0].provenance.loopPassOrdinal == 1);
    REQUIRE (current->previousTakes[1].provenance.capturedAtMs == 91);
    REQUIRE (current->previousTakes[1].lengthInTicks == 3);
    REQUIRE (current->previousTakes[1].notes.size() == 1);
    REQUIRE (current->previousTakes[1].notes[0].lengthInTicks == 3);
    REQUIRE (current->previousTakes[2].provenance.capturedAtMs == 90);
    REQUIRE (current->previousTakes[2].lengthInTicks == 3);
    REQUIRE (current->previousTakes[2].notes.size() == 1);
    REQUIRE (current->previousTakes[2].notes[0].lengthInTicks == 3);
    REQUIRE (std::any_of (regions.begin(), regions.end(), [] (const MidiRegion& r)
    {
        return r.provenance.capturedAtMs == 91 && r.lengthInSamples == 8;
    }));
}

TEST_CASE ("Loop commit diff snapshots provenance and take history for undo",
           "[recording][recordmanager][loop-takes][undo]")
{
    const auto temp = makeSessionDir ("dusk-loop-diff-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    AudioRegion before;
    before.file = temp.dir.getChildFile ("before.wav");
    before.timelineStart = 100;
    before.lengthInSamples = 8;
    before.provenance = { 55, 0, false };
    session.track (0).regions.push_back (before);

    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));
    writeAudioPass (manager, 1, 100, 8);
    writeAudioPass (manager, 2, 100, 8);
    manager.stopRecording (108);

    const auto& diff = manager.getLastCommitDiff();
    REQUIRE (diff.size() == 1);
    REQUIRE (diff[0].trackIndex == 0);
    REQUIRE (diff[0].audioBefore.size() == 1);
    REQUIRE (diff[0].audioBefore[0].provenance.capturedAtMs == 55);
    REQUIRE (diff[0].audioAfter.size() == 1);
    REQUIRE (diff[0].audioAfter[0].provenance.loopPassOrdinal == 2);
    REQUIRE (diff[0].audioAfter[0].previousTakes.size() == 2);
    REQUIRE (diff[0].audioAfter[0].previousTakes[0].provenance.loopPassOrdinal == 1);
    REQUIRE (diff[0].audioAfter[0].previousTakes[1].provenance.capturedAtMs == 55);
}

TEST_CASE ("Loop capture plan rejects an empty effective punch intersection",
           "[recording][recordmanager][loop-takes][punch]")
{
    const auto temp = makeSessionDir ("dusk-loop-empty-plan-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);
    auto plan = loopPlan();
    plan.captureStartSample = plan.captureEndSample;
    REQUIRE_FALSE (manager.startRecording (48000.0, 100, 0, plan));

    plan = loopPlan();
    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));
    const auto preRoll = manager.coordinateLoopCaptureSpan (1, 90, 0, 8);
    REQUIRE (preRoll.numSamples == 0);
    manager.stopRecording (100);
    REQUIRE (session.track (0).regions.empty());
}

TEST_CASE ("Loop capture snapshots and writes only the effective punch intersection",
           "[recording][recordmanager][loop-takes][punch]")
{
    const auto temp = makeSessionDir ("dusk-loop-punch-intersection-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);
    auto plan = loopPlan (100, 120);
    plan.captureStartSample = 105;
    plan.captureEndSample = 115;
    REQUIRE (manager.startRecording (48000.0, 105, 0, plan));

    plan.captureStartSample = 100;
    REQUIRE (manager.getLoopCapturePlan().captureStartSample == 105);

    std::vector<float> firstBlock (8, 0.25f);
    auto span = manager.coordinateLoopCaptureSpan (1, 100, 0, 8);
    REQUIRE (span.inputOffset == 5);
    REQUIRE (span.numSamples == 3);
    REQUIRE (span.startsPass);
    REQUIRE_FALSE (span.endsPass);
    manager.beginLoopCaptureSpan (span);
    manager.writeInputBlock (0, firstBlock.data() + span.inputOffset, nullptr,
                             span.numSamples);

    std::vector<float> secondBlock (12, 0.5f);
    span = manager.coordinateLoopCaptureSpan (1, 108, 0, 12);
    REQUIRE (span.inputOffset == 0);
    REQUIRE (span.numSamples == 7);
    REQUIRE_FALSE (span.startsPass);
    REQUIRE (span.endsPass);
    manager.beginLoopCaptureSpan (span);
    manager.writeInputBlock (0, secondBlock.data(), nullptr, span.numSamples);
    manager.stopRecording (115);

    const auto& region = loopAudioRegion (session);
    REQUIRE (region.timelineStart == 105);
    REQUIRE (region.lengthInSamples == 10);
    REQUIRE (region.sourceOffset == 0);
    REQUIRE_FALSE (region.provenance.partialPass);
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/RecordManager.h"
#include "engine/audiofile/FileReader.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace duskstudio;

namespace
{
void addNoteOn (dusk::MidiBuffer& events, int channel, int note,
                std::uint8_t velocity, int sample)
{
    const std::uint8_t bytes[] {
        (std::uint8_t) (0x90 | (channel - 1)), (std::uint8_t) note, velocity };
    events.addEvent (bytes, 3, sample);
}

void addNoteOff (dusk::MidiBuffer& events, int channel, int note, int sample)
{
    const std::uint8_t bytes[] {
        (std::uint8_t) (0x80 | (channel - 1)), (std::uint8_t) note, 0 };
    events.addEvent (bytes, 3, sample);
}

void addController (dusk::MidiBuffer& events, int channel, int controller,
                    std::uint8_t value, int sample)
{
    const std::uint8_t bytes[] {
        (std::uint8_t) (0xB0 | (channel - 1)), (std::uint8_t) controller, value };
    events.addEvent (bytes, 3, sample);
}

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
                    dusk::MidiBuffer events,
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

dusk::MidiBuffer oneNote (int note, int onSample = 1, int offSample = 6)
{
    dusk::MidiBuffer events;
    addNoteOn (events, 1, note, 100, onSample);
    addNoteOff (events, 1, note, offSample);
    return events;
}

const AudioRegion& regionOfTake (const Session& session, TakeId id)
{
    const auto& regions = session.track (0).regions;
    const auto it = std::find_if (regions.begin(), regions.end(), [id] (const AudioRegion& region)
    {
        return region.takeId == id;
    });
    REQUIRE (it != regions.end());
    return *it;
}

AudioTake seedTake (Session& session, const juce::File& file, std::int64_t timelineStart,
                    std::int64_t length, std::int64_t sourceOffset, std::int64_t capturedAtMs)
{
    AudioTake take;
    take.id = session.allocateTakeId();
    take.name = "Take " + std::to_string (session.track (0).takes.size() + 1);
    take.file = file;
    take.timelineStart = timelineStart;
    take.lengthInSamples = length;
    take.sourceOffset = sourceOffset;
    take.provenance.capturedAtMs = capturedAtMs;
    session.track (0).takes.push_back (take);
    return take;
}

AudioRegion regionFromTake (const AudioTake& take)
{
    AudioRegion region;
    region.file = take.file;
    region.timelineStart = take.timelineStart;
    region.lengthInSamples = take.lengthInSamples;
    region.sourceOffset = take.sourceOffset;
    region.provenance = take.provenance;
    region.takeId = take.id;
    return region;
}

void recordLinear (RecordManager& manager, std::int64_t start, int samples)
{
    REQUIRE (manager.startRecording (48000.0, start, 0));
    std::vector<float> block ((size_t) samples, 0.25f);
    manager.writeInputBlock (0, block.data(), nullptr, samples);
    manager.stopRecording (start + samples);
}

bool takeMatches (const AudioTake& take, std::int64_t timelineStart,
                  std::int64_t length, std::int64_t sourceOffset)
{
    return take.timelineStart == timelineStart && take.lengthInSamples == length
        && take.sourceOffset == sourceOffset && take.file.existsAsFile();
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

    REQUIRE (session.track (0).regions.size() == 1);
    const auto& region = loopAudioRegion (session);
    REQUIRE (region.timelineStart == 100);
    REQUIRE (region.sourceOffset == 8);
    REQUIRE (region.lengthInSamples == 8);
    REQUIRE (region.provenance.loopPassOrdinal == 2);
    REQUIRE_FALSE (region.provenance.partialPass);
    REQUIRE (region.provenance.capturedAtMs > 0);

    const auto& takes = session.track (0).takes;
    REQUIRE (takes.size() == 2);
    REQUIRE (takes[0].name == "Take 1");
    REQUIRE (takes[0].file == region.file);
    REQUIRE (takes[0].timelineStart == 100);
    REQUIRE (takes[0].sourceOffset == 0);
    REQUIRE (takes[0].lengthInSamples == 8);
    REQUIRE (takes[0].numChannels == 1);
    REQUIRE (takes[0].provenance.loopPassOrdinal == 1);
    REQUIRE (takes[0].provenance.capturedAtMs == region.provenance.capturedAtMs);
    REQUIRE (takes[1].name == "Take 2");
    REQUIRE (takes[1].id == region.takeId);
    REQUIRE (takes[1].file == region.file);
    REQUIRE (takes[1].timelineStart == 100);
    REQUIRE (takes[1].sourceOffset == 8);
    REQUIRE (takes[1].lengthInSamples == 8);
    REQUIRE (takes[1].provenance.loopPassOrdinal == 2);
    REQUIRE (takes[0].id != takes[1].id);
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
        const auto& takes = session.track (0).takes;
        REQUIRE (takes.size() == 2);
        REQUIRE (takeMatches (takes[0], 100, 8, 0));
        REQUIRE_FALSE (takes[0].provenance.partialPass);
        REQUIRE (takeMatches (takes[1], 100, 3, 8));
        REQUIRE (takes[1].provenance.partialPass);
        REQUIRE (takes[1].id == region.takeId);
    }

    SECTION ("exact loop boundary")
    {
        writeAudioPass (manager, 2, 100, 8);
        manager.stopRecording (108);
        const auto& region = loopAudioRegion (session);
        REQUIRE (region.provenance.loopPassOrdinal == 2);
        REQUIRE_FALSE (region.provenance.partialPass);
        const auto& takes = session.track (0).takes;
        REQUIRE (takes.size() == 2);
        REQUIRE_FALSE (takes[1].provenance.partialPass);
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

    const auto& takes = session.track (0).takes;
    REQUIRE (takes.size() == 1);
    REQUIRE (takes[0].id == region.takeId);
    REQUIRE (takes[0].provenance.loopPassOrdinal == 2);
    REQUIRE (takes[0].sourceOffset == 1);
    REQUIRE (takes[0].lengthInSamples == 4);

    const auto& errors = manager.getLastRecordErrors();
    REQUIRE (errors.size() == 1);
    REQUIRE (errors[0].kind == RecordManager::RecordErrorKind::WavWrite);
    REQUIRE (errors[0].count == 1);
}

TEST_CASE ("Loop audio keeps every pass as a take with no per-region history cap",
           "[recording][recordmanager][loop-takes][history]")
{
    const auto temp = makeSessionDir ("dusk-loop-audio-every-pass-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);

    for (int i = 0; i < 8; ++i)
        seedTake (session, temp.dir.getChildFile ("earlier-" + juce::String (i) + ".wav"),
                  100, 8, 0, 70 + i);
    session.track (0).regions.push_back (regionFromTake (session.track (0).takes.back()));
    const auto earlier = session.track (0).takes;

    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));
    constexpr int passes = 20;
    for (int ordinal = 1; ordinal <= passes; ++ordinal)
        writeAudioPass (manager, ordinal, 100, 8);
    manager.stopRecording (108);

    const auto& takes = session.track (0).takes;
    REQUIRE (takes.size() == earlier.size() + passes);
    for (size_t i = 0; i < earlier.size(); ++i)
    {
        REQUIRE (takes[i].id == earlier[i].id);
        REQUIRE (takes[i].file == earlier[i].file);
        REQUIRE (takes[i].lengthInSamples == 8);
    }
    const auto spool = takes[earlier.size()].file;
    for (int pass = 0; pass < passes; ++pass)
    {
        const auto& take = takes[earlier.size() + (size_t) pass];
        REQUIRE (take.provenance.loopPassOrdinal == pass + 1);
        REQUIRE_FALSE (take.provenance.partialPass);
        REQUIRE (take.file == spool);
        REQUIRE (takeMatches (take, 100, 8, 8 * pass));
        REQUIRE (take.name == "Take " + std::to_string (earlier.size() + (size_t) pass + 1));
    }
    REQUIRE (manager.getLastRecordErrors().empty());

    const auto& regions = session.track (0).regions;
    REQUIRE (regions.size() == 1);
    REQUIRE (regions[0].takeId == takes.back().id);
}

TEST_CASE ("Loop audio past the pass limit leaves the extra passes out and keeps every earlier pass",
           "[recording][recordmanager][loop-takes][failure]")
{
    const auto temp = makeSessionDir ("dusk-loop-audio-pass-limit-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));

    constexpr int limit = RecordManager::kMaxLoopPassesPerGesture;
    for (int ordinal = 1; ordinal <= limit; ++ordinal)
        writeAudioPass (manager, ordinal, 100, 8);
    // Two passes over the limit, each arriving as two callbacks.
    for (int ordinal = limit + 1; ordinal <= limit + 2; ++ordinal)
    {
        writeAudioPass (manager, ordinal, 100, 4, 0.75f);
        writeAudioPass (manager, ordinal, 104, 4, 0.75f);
    }
    manager.stopRecording (108);

    const auto& takes = session.track (0).takes;
    REQUIRE (takes.size() == (size_t) limit);
    for (int pass = 0; pass < limit; ++pass)
    {
        const auto& take = takes[(size_t) pass];
        REQUIRE (take.provenance.loopPassOrdinal == pass + 1);
        REQUIRE_FALSE (take.provenance.partialPass);
        REQUIRE (takeMatches (take, 100, 8, 8 * pass));
    }

    const auto& region = loopAudioRegion (session);
    REQUIRE (region.takeId == takes.back().id);
    REQUIRE (region.provenance.loopPassOrdinal == limit);

    const auto& errors = manager.getLastRecordErrors();
    REQUIRE (errors.size() == 1);
    REQUIRE (errors[0].trackIndex == 0);
    REQUIRE (errors[0].kind == RecordManager::RecordErrorKind::LoopPassLimit);
    REQUIRE (errors[0].count == 2);

    const auto reader = dusk::audio::FileReader::open (
        takes.back().file.getFullPathName().toStdString());
    REQUIRE (reader != nullptr);
    REQUIRE (reader->info().numFrames == (std::int64_t) limit * 8);
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

    const auto& takes = session.track (0).takes;
    REQUIRE (takes.size() == 2);
    REQUIRE (takeMatches (takes[0], 0, 5, 3));
    REQUIRE (takeMatches (takes[1], 0, 5, 11));
    REQUIRE (takes[1].id == region.takeId);
}

TEST_CASE ("A take the latency trim consumes leaves no take and no file",
           "[recording][recordmanager][latency]")
{
    const auto temp = makeSessionDir ("dusk-linear-audio-consumed-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);
    REQUIRE (manager.startRecording (48000.0, 0, 16));
    std::vector<float> block (8, 0.25f);
    manager.writeInputBlock (0, block.data(), nullptr, 8);
    manager.stopRecording (8);

    REQUIRE (session.track (0).takes.empty());
    REQUIRE (session.track (0).regions.empty());
    REQUIRE (manager.getLastCommitDiff().empty());
    REQUIRE (temp.dir.getChildFile ("audio").findChildFiles (juce::File::findFiles, false, "*.wav").isEmpty());
    const auto& errors = manager.getLastRecordErrors();
    REQUIRE (errors.size() == 1);
    REQUIRE (errors[0].kind == RecordManager::RecordErrorKind::OffsetConsumedTake);
}

TEST_CASE ("Loop punch inside a longer region keeps its take whole and splits it around the new one",
           "[recording][recordmanager][loop-takes][punch]")
{
    const auto temp = makeSessionDir ("dusk-loop-audio-span-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);

    const auto spanning = seedTake (session, temp.dir.getChildFile ("spanning.wav"), 90, 30, 50, 42);
    session.track (0).regions.push_back (regionFromTake (spanning));

    RecordManager manager (session);
    const auto plan = loopPlan (100, 110);
    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));
    writeAudioPass (manager, 1, 100, 10);
    manager.stopRecording (110);

    const auto& takes = session.track (0).takes;
    REQUIRE (takes.size() == 2);
    REQUIRE (takes[0].id == spanning.id);
    REQUIRE (takes[0].timelineStart == 90);
    REQUIRE (takes[0].lengthInSamples == 30);
    REQUIRE (takes[0].sourceOffset == 50);
    REQUIRE (takeMatches (takes[1], 100, 10, 0));

    const auto& regions = session.track (0).regions;
    REQUIRE (regions.size() == 3);
    constexpr std::int64_t fade = 5;
    const auto& punched = loopAudioRegion (session);
    REQUIRE (punched.takeId == takes[1].id);
    REQUIRE (punched.timelineStart == 100);
    REQUIRE (punched.lengthInSamples == 10);
    REQUIRE (punched.fadeInSamples == fade);
    REQUIRE (punched.fadeOutSamples == fade);

    const auto left = std::find_if (regions.begin(), regions.end(), [] (const AudioRegion& r)
    {
        return r.timelineStart == 90;
    });
    REQUIRE (left != regions.end());
    REQUIRE (left->takeId == spanning.id);
    REQUIRE (left->sourceOffset == 50);
    REQUIRE (left->lengthInSamples == 10 + fade);
    REQUIRE (left->fadeOutSamples == fade);
    REQUIRE (left->fadeOutShape == FadeShape::RaisedCosine);

    const auto right = std::find_if (regions.begin(), regions.end(), [] (const AudioRegion& r)
    {
        return r.timelineStart == 110 - fade;
    });
    REQUIRE (right != regions.end());
    REQUIRE (right->takeId == spanning.id);
    REQUIRE (right->sourceOffset == 50 + 20 - fade);
    REQUIRE (right->timelineStart + right->lengthInSamples == 120);
    REQUIRE (right->fadeInSamples == fade);
    REQUIRE (right->fadeInShape == FadeShape::RaisedCosine);
}

TEST_CASE ("Final partial audio pass over a region keeps both passes and the region's take whole",
           "[recording][recordmanager][loop-takes][punch]")
{
    const auto temp = makeSessionDir ("dusk-loop-audio-partial-history-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);

    const auto existing = seedTake (session, temp.dir.getChildFile ("existing-full.wav"),
                                    100, 8, 40, 81);
    session.track (0).regions.push_back (regionFromTake (existing));

    RecordManager manager (session);
    const auto plan = loopPlan();
    REQUIRE (manager.startRecording (48000.0, 100, 0, plan));
    writeAudioPass (manager, 1, 100, 8);
    writeAudioPass (manager, 2, 100, 5);
    manager.stopRecording (105);

    const auto& takes = session.track (0).takes;
    REQUIRE (takes.size() == 3);
    REQUIRE (takes[0].id == existing.id);
    REQUIRE (takes[0].sourceOffset == 40);
    REQUIRE (takes[0].lengthInSamples == 8);
    REQUIRE (takeMatches (takes[1], 100, 8, 0));
    REQUIRE (takes[1].provenance.loopPassOrdinal == 1);
    REQUIRE (takeMatches (takes[2], 100, 5, 8));
    REQUIRE (takes[2].provenance.partialPass);

    const auto& region = loopAudioRegion (session);
    REQUIRE (region.provenance.loopPassOrdinal == 2);
    REQUIRE (region.takeId == takes[2].id);

    const auto& regions = session.track (0).regions;
    REQUIRE (regions.size() == 2);
    const auto& tail = regionOfTake (session, existing.id);
    REQUIRE (tail.timelineStart < 105);
    REQUIRE (tail.timelineStart + tail.lengthInSamples == 108);
    REQUIRE (tail.sourceOffset == 40 + (tail.timelineStart - 100));
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

    constexpr int firstOrdinal = 99981;
    constexpr int lastOrdinal = 100000;
    for (int ordinal = firstOrdinal; ordinal <= lastOrdinal; ++ordinal)
    {
        auto events = oneNote (60 + ordinal - firstOrdinal);
        if (ordinal == firstOrdinal)
            addController (events, 1, 64, 127, 2);
        writeMidiPass (manager, ordinal, 100, std::move (events), 8);
    }
    manager.stopRecording (108);

    const auto region = session.track (0).midiRegions.current().at (0);
    REQUIRE (region.provenance.loopPassOrdinal == lastOrdinal);
    REQUIRE (region.previousTakes.size() == 8);
    for (int i = 0; i < 8; ++i)
        REQUIRE (region.previousTakes[(size_t) i].provenance.loopPassOrdinal
                 == lastOrdinal - 1 - i);

    // The sustain was pressed in a pass outside the kept nine; the oldest
    // kept take still starts with it.
    const auto& oldestKept = region.previousTakes.back();
    REQUIRE_FALSE (oldestKept.ccs.empty());
    REQUIRE (oldestKept.ccs.front().controller == 64);
    REQUIRE (oldestKept.ccs.front().value == 127);
    REQUIRE (oldestKept.ccs.front().atTick == 0);
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

    dusk::MidiBuffer first;
    addNoteOn (first, 1, 67, 91, 6);
    writeMidiPass (manager, 1, 100, first, 8);
    dusk::MidiBuffer second;
    addNoteOff (second, 1, 67, 2);
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

    dusk::MidiBuffer first;
    addController (first, 1, 64, 127, 2);
    writeMidiPass (manager, 1, 100, first, 8);
    dusk::MidiBuffer second;
    addController (second, 1, 64, 0, 3);
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
    dusk::MidiBuffer events;
    events.addEvent (invalidNoteData, 3, 1);
    events.addEvent (invalidCcData, 3, 2);
    addNoteOn (events, 1, 64, 100, 4);
    events.addEvent (truncatedNote, 2, 5);
    addNoteOff (events, 1, 64, 6);
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

    dusk::MidiBuffer oldPass;
    for (int sample = 0; sample < capacity; ++sample)
        addController (oldPass, 1, 1, 1, sample);
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

    dusk::MidiBuffer originalCallback;
    addNoteOn (originalCallback, 1, 50, 100, 1);
    addNoteOff (originalCallback, 1, 50, 3);
    addNoteOn (originalCallback, 1, 60, 100, 5);
    addNoteOff (originalCallback, 1, 60, 7);
    addNoteOn (originalCallback, 1, 70, 100, 8);
    addNoteOff (originalCallback, 1, 70, 10);
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

    dusk::MidiBuffer originalCallback;
    addNoteOn (originalCallback, 1, 60, 100, 0);
    addNoteOff (originalCallback, 1, 60, 1);
    addNoteOn (originalCallback, 1, 64, 100, 4);
    addNoteOff (originalCallback, 1, 64, 5);

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

TEST_CASE ("Loop commit diff snapshots provenance and takes for undo",
           "[recording][recordmanager][loop-takes][undo]")
{
    const auto temp = makeSessionDir ("dusk-loop-diff-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    const auto before = seedTake (session, temp.dir.getChildFile ("before.wav"), 100, 8, 0, 55);
    session.track (0).regions.push_back (regionFromTake (before));

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
    REQUIRE (diff[0].audioBefore[0].takeId == before.id);
    REQUIRE (diff[0].takesBefore.size() == 1);
    REQUIRE (diff[0].takesBefore[0].id == before.id);
    REQUIRE (diff[0].takesAfter.size() == 3);
    REQUIRE (diff[0].takesAfter[0].id == before.id);
    REQUIRE (diff[0].takesAfter[1].provenance.loopPassOrdinal == 1);
    REQUIRE (diff[0].takesAfter[2].provenance.loopPassOrdinal == 2);
    REQUIRE (diff[0].audioAfter.size() == 1);
    REQUIRE (diff[0].audioAfter[0].provenance.loopPassOrdinal == 2);
    REQUIRE (diff[0].audioAfter[0].takeId == diff[0].takesAfter[2].id);
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

TEST_CASE ("Three takes recorded over each other all stay complete on the track",
           "[recording][recordmanager][takes][regression]")
{
    const auto temp = makeSessionDir ("dusk-overlapping-takes-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);

    recordLinear (manager, 1000, 4000);
    recordLinear (manager, 3000, 4000);
    recordLinear (manager, 2000, 2000);

    const auto& takes = session.track (0).takes;
    REQUIRE (takes.size() == 3);
    REQUIRE (takeMatches (takes[0], 1000, 4000, 0));
    REQUIRE (takeMatches (takes[1], 3000, 4000, 0));
    REQUIRE (takeMatches (takes[2], 2000, 2000, 0));
    REQUIRE (takes[0].file != takes[1].file);
    REQUIRE (takes[1].file != takes[2].file);
    REQUIRE (takes[0].name == "Take 1");
    REQUIRE (takes[1].name == "Take 2");
    REQUIRE (takes[2].name == "Take 3");

    const auto& regions = session.track (0).regions;
    REQUIRE (regions.size() == 3);
    const auto& first = regionOfTake (session, takes[0].id);
    REQUIRE (first.timelineStart == 1000);
    REQUIRE (first.timelineStart + first.lengthInSamples == 2000 + 64);
    const auto& second = regionOfTake (session, takes[1].id);
    REQUIRE (second.timelineStart == 4000 - 64);
    REQUIRE (second.sourceOffset == 4000 - 64 - 3000);
    REQUIRE (second.timelineStart + second.lengthInSamples == 7000);
    const auto& third = regionOfTake (session, takes[2].id);
    REQUIRE (third.timelineStart == 2000);
    REQUIRE (third.lengthInSamples == 2000);
}

TEST_CASE ("Deleting the last-recorded region removes no take",
           "[recording][recordmanager][takes][regression]")
{
    const auto temp = makeSessionDir ("dusk-delete-last-take-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);

    recordLinear (manager, 1000, 4000);
    recordLinear (manager, 1000, 4000);
    auto& regions = session.track (0).regions;
    REQUIRE (regions.size() == 1);
    const auto newest = regions[0].takeId;
    REQUIRE (newest == session.track (0).takes.back().id);

    regions.clear();

    const auto& takes = session.track (0).takes;
    REQUIRE (takes.size() == 2);
    REQUIRE (takeMatches (takes[0], 1000, 4000, 0));
    REQUIRE (takeMatches (takes[1], 1000, 4000, 0));
    REQUIRE (takes[1].id == newest);
}

TEST_CASE ("A take punched into a recorded region leaves both pieces naming the older take",
           "[recording][recordmanager][takes][regression]")
{
    const auto temp = makeSessionDir ("dusk-split-recorded-take-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);

    recordLinear (manager, 1000, 4000);
    recordLinear (manager, 1000, 4000);
    recordLinear (manager, 2000, 1000);

    const auto& regions = session.track (0).regions;
    REQUIRE (regions.size() == 3);
    for (const auto& region : regions)
        REQUIRE (region.takeId != 0);
    const auto& takes = session.track (0).takes;
    REQUIRE (takes.size() == 3);
    REQUIRE (regionOfTake (session, takes[2].id).timelineStart == 2000);
    const auto split = takes[1].id;
    REQUIRE (std::count_if (regions.begin(), regions.end(), [split] (const AudioRegion& r)
    {
        return r.takeId == split;
    }) == 2);
}

TEST_CASE ("A take recorded over the end of a region never stretches that region past its own audio",
           "[recording][recordmanager][takes][punch][regression]")
{
    const auto temp = makeSessionDir ("dusk-record-seam-");
    Session session;
    armTrack (session, temp.dir, Track::Mode::Mono);
    RecordManager manager (session);
    constexpr std::int64_t fade = 64;

    SECTION ("a region ending inside the seam stops where its loop pass does")
    {
        REQUIRE (manager.startRecording (48000.0, 0, 0, loopPlan (0, 100)));
        writeAudioPass (manager, 1, 0, 100, 0.25f);
        writeAudioPass (manager, 2, 0, 100, 0.75f);
        manager.stopRecording (100);
        const auto firstPass = session.track (0).takes[0];
        session.track (0).regions.assign (1, regionFromTake (firstPass));

        recordLinear (manager, 90, 200);

        const auto& kept = regionOfTake (session, firstPass.id);
        CHECK (kept.timelineStart == 0);
        CHECK (kept.sourceOffset == 0);
        CHECK (kept.lengthInSamples == 100);
        CHECK (kept.fadeOutSamples == 10);
        CHECK (kept.fadeOutShape == FadeShape::RaisedCosine);

        const auto reader = dusk::audio::FileReader::open (
            kept.file.getFullPathName().toStdString());
        REQUIRE (reader != nullptr);
        std::vector<float> played ((size_t) kept.lengthInSamples);
        float* dest[] { played.data() };
        REQUIRE (reader->read (dest, 1, kept.sourceOffset, kept.lengthInSamples)
                 == kept.lengthInSamples);
        const auto [quietest, loudest] = std::minmax_element (played.begin(), played.end());
        CHECK_THAT (*quietest, Catch::Matchers::WithinAbs (0.25f, 1e-4));
        CHECK_THAT (*loudest, Catch::Matchers::WithinAbs (0.25f, 1e-4));
    }

    SECTION ("a region starting inside the seam keeps its own start and source offset")
    {
        recordLinear (manager, 1000, 2000);
        const auto older = session.track (0).takes[0];

        recordLinear (manager, 0, 1020);

        const auto& kept = regionOfTake (session, older.id);
        CHECK (kept.timelineStart == 1000);
        CHECK (kept.sourceOffset == 0);
        CHECK (kept.lengthInSamples == 2000);
        CHECK (kept.fadeInSamples == 20);
        CHECK (kept.fadeInShape == FadeShape::RaisedCosine);
    }

    SECTION ("a region trimmed at one edge shrinks its far fade so the two never overlap")
    {
        auto left = regionFromTake (
            seedTake (session, temp.dir.getChildFile ("left.wav"), 1000, 200, 0, 11));
        left.fadeInSamples = 150;
        auto right = regionFromTake (
            seedTake (session, temp.dir.getChildFile ("right.wav"), 1900, 300, 0, 12));
        right.fadeOutSamples = 250;
        session.track (0).regions = { left, right };

        recordLinear (manager, 1100, 900);

        const auto& keptLeft = regionOfTake (session, left.takeId);
        CHECK (keptLeft.lengthInSamples == 100 + fade);
        CHECK (keptLeft.fadeOutSamples == fade);
        CHECK (keptLeft.fadeInSamples == 100);

        const auto& keptRight = regionOfTake (session, right.takeId);
        CHECK (keptRight.timelineStart == 2000 - fade);
        CHECK (keptRight.lengthInSamples == 200 + fade);
        CHECK (keptRight.fadeInSamples == fade);
        CHECK (keptRight.fadeOutSamples == 200);
    }
}

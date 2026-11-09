#include <catch2/catch_test_macros.hpp>

#include <juce_core/juce_core.h>

#include "TestTempDirectory.h"
#include "engine/RecordManager.h"
#include "engine/midi/MidiFileReader.h"
#include "session/MidiRecovery.h"
#include "session/MidiRecoveryWriter.h"
#include "session/Session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace duskstudio;
namespace stdfs = std::filesystem;

namespace
{
// At 120 bpm, 960 samples a second put one tick on each sample.
constexpr double kRate = 960.0;
constexpr std::int64_t kNovember8th143005 = 1794148205;   // 2026-11-08 14:30:05 UTC

struct Recording
{
    duskstudio::test::TempDirectory folder { "dusk-midi-take-files-" };
    Session session;
    RecordManager manager { session };

    explicit Recording (bool merge = false)
    {
        session.setSessionDirectory (juce::File (juce::String (folder.path().u8string())));
        session.tempoBpm.store (120.0f, std::memory_order_relaxed);
        session.midiMergeEnabled.store (merge, std::memory_order_relaxed);
        session.track (0).name = "Keys";
        session.track (0).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
        session.setTrackArmed (0, true);
    }
};

RecordManager::LoopCapturePlan loopPlan()
{
    return { true, 100, 200, 100, 200 };
}

void addMessage (dusk::MidiBuffer& buffer, std::uint8_t status,
                 std::uint8_t data1, std::uint8_t data2, int sample)
{
    const std::uint8_t bytes[] { status, data1, data2 };
    buffer.addEvent (bytes, 3, sample);
}

dusk::MidiBuffer note (int pitch, int onset = 10, int release = 30)
{
    dusk::MidiBuffer buffer;
    addMessage (buffer, 0x90, (std::uint8_t) pitch, 100, onset);
    addMessage (buffer, 0x80, (std::uint8_t) pitch, 0, release);
    return buffer;
}

void writeLoop (RecordManager& manager, int ordinal, const dusk::MidiBuffer& buffer, int length = 100)
{
    const auto span = manager.coordinateLoopCaptureSpan (ordinal, 100, 0, length);
    REQUIRE (span.passOrdinal == ordinal);
    REQUIRE (span.numSamples == length);
    manager.beginLoopCaptureSpan (span);
    manager.writeMidiBlock (0, buffer, 0, &span, length);
}

std::vector<int> ordinalsOf (const std::vector<RecordManager::RecordedMidiPass>& recorded)
{
    std::vector<int> ordinals;
    for (const auto& r : recorded)
        ordinals.push_back (r.pass.provenance.loopPassOrdinal);
    return ordinals;
}

// The channel messages of a file as { tick, status, data1, data2 }, by tick.
std::vector<std::array<std::int64_t, 4>> eventsIn (const stdfs::path& file)
{
    midi::MidiFileReader reader;
    REQUIRE (reader.readFile (file));
    REQUIRE (reader.timeFormat() == kMidiTicksPerQuarter);
    std::vector<std::array<std::int64_t, 4>> events;
    for (const auto& track : reader.tracks())
        for (const auto& event : track)
            if (event.isChannelMessage())
                events.push_back ({ event.tick, event.status, event.data1, event.data2 });
    std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b) { return a[0] < b[0]; });
    return events;
}

std::vector<std::string> namesIn (const stdfs::path& folder)
{
    std::vector<std::string> names;
    for (const auto& entry : stdfs::directory_iterator (folder))
        names.push_back (entry.path().filename().u8string());
    std::sort (names.begin(), names.end());
    return names;
}

// What the engine does with the passes a stop hands over: a take file for
// each, queued for the writer, which is flushed before the disk is read.
void writeTakeFiles (Recording& recording, const std::vector<RecordManager::RecordedMidiPass>& recorded)
{
    const auto now = std::chrono::system_clock::time_point (std::chrono::seconds (kNovember8th143005));
    std::vector<midirecovery::WriteResult> results;
    midirecovery::Writer writer ([&results] (const midirecovery::PendingFile&, const midirecovery::WriteResult& result)
                                 { results.push_back (result); });
    for (const auto& r : recorded)
    {
        auto file = midirecovery::pendingTakeFile (recording.folder.path(), now, recording.session, kRate,
                                                   r.trackIndex, r.pass);
        REQUIRE (file.has_value());
        writer.queue (std::move (*file));
    }
    writer.flush();
    REQUIRE (results.size() == recorded.size());
    for (const auto& result : results)
    {
        REQUIRE (result.failures.empty());
        REQUIRE (result.written.size() == 1);
    }
}
} // namespace

TEST_CASE ("A linear MIDI take hands over its one new pass and none of the regions it covers",
           "[recording][recordmanager][midi][recovery]")
{
    Recording recording;
    MidiRegion covered;
    covered.timelineStart = 1100;
    covered.lengthInSamples = covered.lengthInTicks = 50;
    covered.notes.push_back ({ 1, 48, 90, 0, 20 });
    recording.session.track (0).midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (1, covered));

    REQUIRE (recording.manager.startRecording (kRate, 1000));
    recording.manager.writeMidiBlock (0, note (60), 0, nullptr, 100);
    recording.manager.stopRecording (1200);

    const auto regions = recording.session.track (0).midiRegions.current();
    REQUIRE (regions.size() == 1);
    REQUIRE (regions[0].previousTakes.size() == 1);

    const auto recorded = recording.manager.takeRecordedMidiPasses();
    REQUIRE (recorded.size() == 1);
    CHECK (recorded[0].trackIndex == 0);
    CHECK (recorded[0].pass.timelineStart == 1000);
    CHECK (recorded[0].pass.lengthInSamples == 200);
    CHECK (recorded[0].pass.notes == std::vector<MidiNote> { { 1, 60, 100, 10, 20 } });
    CHECK (recorded[0].pass.notes == regions[0].notes);
    CHECK (recorded[0].pass.previousTakes.empty());
    CHECK (recording.manager.takeRecordedMidiPasses().empty());

    SECTION ("a take with nothing in it hands over nothing")
    {
        REQUIRE (recording.manager.startRecording (kRate, 2000));
        recording.manager.writeMidiBlock (0, {}, 0, nullptr, 100);
        recording.manager.stopRecording (2100);
        CHECK (recording.manager.takeRecordedMidiPasses().empty());
    }
}

TEST_CASE ("A loop MIDI take hands over every pass it recorded, the ones too old to stay as takes included",
           "[recording][recordmanager][loop-takes][midi][recovery]")
{
    Recording recording;
    REQUIRE (recording.manager.startRecording (kRate, 100, 0, loopPlan()));
    for (int ordinal = 1; ordinal <= 12; ++ordinal)
        writeLoop (recording.manager, ordinal, ordinal == 2 || ordinal == 5 ? dusk::MidiBuffer {}
                                                                            : note (59 + ordinal));
    recording.manager.stopRecording (200);

    const auto recorded = recording.manager.takeRecordedMidiPasses();
    REQUIRE (ordinalsOf (recorded) == std::vector<int> { 1, 3, 4, 6, 7, 8, 9, 10, 11, 12 });
    for (const auto& r : recorded)
    {
        CAPTURE (r.pass.provenance.loopPassOrdinal);
        CHECK (r.trackIndex == 0);
        CHECK (r.pass.timelineStart == 100);
        CHECK (r.pass.lengthInSamples == 100);
        CHECK (r.pass.notes == std::vector<MidiNote> {
            { 1, 59 + r.pass.provenance.loopPassOrdinal, 100, 10, 20 } });
        CHECK (r.pass.previousTakes.empty());
    }

    // The newest eight with notes are the region and its previous takes, and
    // the two before them, which the region does not keep, are handed over too.
    const auto region = recording.session.track (0).midiRegions.current().at (0);
    REQUIRE (region.previousTakes.size() == 7);
    CHECK (region.notes == recorded.back().pass.notes);
    for (std::size_t i = 0; i < region.previousTakes.size(); ++i)
        CHECK (region.previousTakes[i].notes == recorded[recorded.size() - 2 - i].pass.notes);
}

TEST_CASE ("A loop MIDI pass too old to stay as a take is handed over with the notes and controls it carried",
           "[recording][recordmanager][loop-takes][midi][recovery]")
{
    Recording recording;
    REQUIRE (recording.manager.startRecording (kRate, 100, 0, loopPlan()));
    auto first = note (60);
    addMessage (first, 0xb0, 64, 127, 5);
    writeLoop (recording.manager, 1, first);
    dusk::MidiBuffer second;
    addMessage (second, 0x90, 67, 91, 80);
    writeLoop (recording.manager, 2, second);
    dusk::MidiBuffer third;
    addMessage (third, 0x80, 67, 0, 20);
    writeLoop (recording.manager, 3, third);
    for (int ordinal = 4; ordinal <= 11; ++ordinal)
        writeLoop (recording.manager, ordinal, note (60 + ordinal));
    recording.manager.stopRecording (200);

    const auto recorded = recording.manager.takeRecordedMidiPasses();
    REQUIRE (ordinalsOf (recorded) == std::vector<int> { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 });

    // The sustain goes down in pass 1 and is let go at each seam; the note
    // held into pass 2 closes at its end and starts pass 3 again.
    CHECK (recorded[0].pass.notes == std::vector<MidiNote> { { 1, 60, 100, 10, 20 } });
    CHECK (recorded[0].pass.ccs == std::vector<MidiCc> { { 1, 64, 127, 5 }, { 1, 64, 0, 99 } });
    CHECK (recorded[1].pass.notes == std::vector<MidiNote> { { 1, 67, 91, 80, 20 } });
    CHECK (recorded[1].pass.ccs == std::vector<MidiCc> { { 1, 64, 127, 0 }, { 1, 64, 0, 99 } });
    CHECK (recorded[2].pass.notes == std::vector<MidiNote> { { 1, 67, 91, 0, 20 } });
    CHECK (recorded[2].pass.ccs == std::vector<MidiCc> { { 1, 64, 127, 0 }, { 1, 64, 0, 99 } });

    const auto region = recording.session.track (0).midiRegions.current().at (0);
    REQUIRE (region.previousTakes.size() == 8);
    const auto& oldestKept = region.previousTakes.back();
    CHECK (oldestKept.provenance.loopPassOrdinal == 3);
    CHECK (oldestKept.notes == recorded[2].pass.notes);
    CHECK (oldestKept.ccs == recorded[2].pass.ccs);
}

TEST_CASE ("MIDI Merge hands over each pass as played, where it started and before it merges",
           "[recording][recordmanager][midi][merge][recovery]")
{
    Recording recording (true);
    MidiRegion existing;
    existing.timelineStart = 100;
    existing.lengthInSamples = existing.lengthInTicks = 100;
    existing.notes.push_back ({ 1, 48, 90, 0, 50 });
    recording.session.track (0).midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (1, existing));

    REQUIRE (recording.manager.startRecording (kRate, 100, 0, loopPlan()));
    writeLoop (recording.manager, 1, note (60));
    writeLoop (recording.manager, 2, note (62, 40, 60));
    auto groups = recording.manager.takeMidiPassGroups();
    dusk::MidiBuffer held;
    addMessage (held, 0x90, 64, 90, 30);
    writeLoop (recording.manager, 3, held, 40);
    recording.manager.stopRecording (140);
    for (auto& group : recording.manager.takeMidiPassGroups())
        groups.push_back (std::move (group));

    REQUIRE (groups.size() == 3);
    std::vector<MidiRegion> played;
    for (const auto& group : groups)
    {
        REQUIRE (group.tracks.size() == 1);
        played.push_back (group.regionOf (group.tracks[0]));
    }
    for (const auto& pass : played)
    {
        CHECK (pass.timelineStart == 100);
        CHECK (pass.ccs.empty());
        CHECK (pass.previousTakes.empty());
    }
    CHECK (played[0].lengthInSamples == 100);
    CHECK (played[0].notes == std::vector<MidiNote> { { 1, 60, 100, 10, 20 } });
    CHECK (played[1].notes == std::vector<MidiNote> { { 1, 62, 100, 40, 20 } });
    CHECK (played[2].lengthInSamples == 40);
    CHECK (played[2].lengthInTicks == 40);
    CHECK (played[2].notes == std::vector<MidiNote> { { 1, 64, 90, 30, 10 } });

    // The engine merges them; the recorder left the region alone and hands
    // over nothing at Stop that a merge pass already carried.
    CHECK (recording.session.track (0).midiRegions.current().at (0).notes == existing.notes);
    CHECK (recording.manager.takeRecordedMidiPasses().empty());
}

TEST_CASE ("Recorded MIDI passes become take files in midi/takes with their events at their ticks",
           "[recording][recordmanager][midi][recovery]")
{
    Recording recording;
    const auto takes = recording.folder.path() / "midi" / "takes";

    SECTION ("a linear take")
    {
        REQUIRE (recording.manager.startRecording (kRate, 1000));
        auto events = note (60, 10, 30);
        addMessage (events, 0xb3, 1, 64, 20);
        recording.manager.writeMidiBlock (0, events, 0, nullptr, 100);
        recording.manager.stopRecording (1100);
        writeTakeFiles (recording, recording.manager.takeRecordedMidiPasses());

        REQUIRE (namesIn (takes) == std::vector<std::string> { "20261108-143005Z_track01_Keys.mid" });
        CHECK (eventsIn (takes / "20261108-143005Z_track01_Keys.mid")
               == std::vector<std::array<std::int64_t, 4>> {
                   { 1010, 0x90, 60, 100 }, { 1020, 0xb3, 1, 64 }, { 1030, 0x80, 60, 0 } });
    }

    SECTION ("a loop take, one file a pass, oldest first")
    {
        REQUIRE (recording.manager.startRecording (kRate, 100, 0, loopPlan()));
        for (int ordinal = 1; ordinal <= 10; ++ordinal)
            writeLoop (recording.manager, ordinal, note (59 + ordinal, ordinal, ordinal + 50));
        recording.manager.stopRecording (200);
        writeTakeFiles (recording, recording.manager.takeRecordedMidiPasses());

        // One second's files sort in the order they were written, pass 1 first.
        std::vector<std::string> names;
        for (int copy = 1; copy <= 10; ++copy)
            names.push_back (midirecovery::fileName (std::chrono::system_clock::time_point (
                                                         std::chrono::seconds (kNovember8th143005)),
                                                     1, "Keys", midirecovery::TrackFile::Playing, copy));
        REQUIRE (namesIn (takes) == names);
        CHECK (names[9] == "20261108-143005Z_track01_Keys_010.mid");
        for (int ordinal = 1; ordinal <= 10; ++ordinal)
        {
            CAPTURE (ordinal);
            CHECK (eventsIn (takes / names[(std::size_t) ordinal - 1]) == std::vector<std::array<std::int64_t, 4>> {
                { 100 + ordinal, 0x90, 59 + ordinal, 100 }, { 150 + ordinal, 0x80, 59 + ordinal, 0 } });
        }
    }
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/RecordManager.h"
#include "session/Session.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSampleRate = 960.0;

struct TempSessionDirectory
{
    juce::File directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("dusk-midi-merge-" + juce::Uuid().toString());
    TempSessionDirectory() { REQUIRE (directory.createDirectory().wasOk()); }
    ~TempSessionDirectory() { directory.deleteRecursively(); }
};

struct CaptureFixture
{
    TempSessionDirectory temporary;
    Session session;
    RecordManager manager { session };

    explicit CaptureFixture (bool merge, int tracks = 1)
    {
        session.setSessionDirectory (temporary.directory);
        session.tempoBpm.store (120.0f, std::memory_order_relaxed);
        session.midiMergeEnabled.store (merge, std::memory_order_relaxed);
        for (int track = 0; track < tracks; ++track)
        {
            session.track (track).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
            session.setTrackArmed (track, true);
        }
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
    buffer.addEvent (bytes, (status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0 ? 2 : 3, sample);
}

dusk::MidiBuffer note (int pitch, int onset = 10, int release = 30)
{
    dusk::MidiBuffer buffer;
    addMessage (buffer, 0x90, (std::uint8_t) pitch, 100, onset);
    addMessage (buffer, 0x80, (std::uint8_t) pitch, 0, release);
    return buffer;
}

dusk::MidiBuffer controls()
{
    dusk::MidiBuffer buffer;
    addMessage (buffer, 0xb1, 64, 127, 1);
    addMessage (buffer, 0xe2, 1, 64, 2);
    addMessage (buffer, 0xd3, 70, 0, 3);
    addMessage (buffer, 0xa4, 60, 80, 4);
    addMessage (buffer, 0xc5, 9, 0, 5);
    return buffer;
}

void writeLoop (RecordManager& manager, int track, int ordinal,
                 const dusk::MidiBuffer& buffer, int length = 100)
{
    const auto span = manager.coordinateLoopCaptureSpan (ordinal, 100, 0, length);
    REQUIRE (span.passOrdinal == ordinal);
    REQUIRE (span.numSamples == length);
    manager.beginLoopCaptureSpan (span);
    manager.writeMidiBlock (track, buffer, 0, &span, length);
}
} // namespace

TEST_CASE ("RecordManager snapshots MIDI merge mode and previews both capture modes",
           "[recording][recordmanager][midi][merge]")
{
    bool merge = false;
    SECTION ("Merge OFF keeps take replacement") {}
    SECTION ("Merge ON returns captured payload without replacing regions") { merge = true; }
    CaptureFixture fixture (merge);
    MidiRegion existing;
    existing.timelineStart = 1000;
    existing.lengthInSamples = existing.lengthInTicks = 100;
    existing.notes.push_back ({ 1, 60, 70, 0, 40 });
    fixture.session.track (0).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (1, existing));
    REQUIRE (fixture.manager.startRecording (kSampleRate, 1000));
    fixture.session.midiMergeEnabled.store (! merge, std::memory_order_relaxed);
    REQUIRE (fixture.manager.isMidiMergeCapture() == merge);
    REQUIRE (fixture.manager.midiCaptureTimelineStart() == 1000);
    dusk::MidiBuffer held;
    addMessage (held, 0x90, 64, 100, 10);
    fixture.manager.writeMidiBlock (0, held, 0, nullptr, 40);
    fixture.manager.serviceMidiCapture();
    const auto* preview = fixture.manager.midiCapturePreview (0);
    REQUIRE (preview != nullptr);
    REQUIRE (preview->lengthInSamples == 40);
    REQUIRE (preview->notes == std::vector<MidiNote> { { 1, 64, 100, 10, 30 } });
    REQUIRE (fixture.session.track (0).midiRegions.current()[0].notes == existing.notes);
    dusk::MidiBuffer empty;
    fixture.manager.writeMidiBlock (0, empty, 40, nullptr, 20);
    fixture.manager.serviceMidiCapture();
    REQUIRE (fixture.manager.midiCapturePreview (0)->notes
             == std::vector<MidiNote> { { 1, 64, 100, 10, 50 } });

    fixture.manager.stopRecording (1100);
    REQUIRE (fixture.manager.isMidiMergeCapture() == merge);
    const auto groups = fixture.manager.takeMidiPassGroups();
    const auto regions = fixture.session.track (0).midiRegions.current();
    REQUIRE (regions.size() == 1);
    if (merge)
    {
        REQUIRE (groups.size() == 1);
        REQUIRE (groups[0].tracks.size() == 1);
        REQUIRE (groups[0].timelineStart == 1000);
        REQUIRE (groups[0].tracks[0].pass.notes
                 == std::vector<MidiNote> { { 1, 64, 100, 10, 90 } });
        REQUIRE (regions[0].notes == existing.notes);
        REQUIRE (regions[0].previousTakes.empty());
        REQUIRE (fixture.manager.getLastCommitDiff().empty());
    }
    else
    {
        REQUIRE (groups.empty());
        REQUIRE (regions[0].notes == std::vector<MidiNote> { { 1, 64, 100, 10, 90 } });
        REQUIRE (regions[0].previousTakes.size() == 1);
        REQUIRE (fixture.manager.getLastCommitDiff().size() == 1);
    }
}

TEST_CASE ("RecordManager waits for every captured MIDI track before publishing a loop group",
           "[recording][recordmanager][midi][merge]")
{
    CaptureFixture fixture (true, 2);
    REQUIRE (fixture.manager.startRecording (kSampleRate, 100, 0, loopPlan()));
    writeLoop (fixture.manager, 0, 1, note (60));
    writeLoop (fixture.manager, 0, 2, note (62));
    REQUIRE (fixture.manager.takeMidiPassGroups().empty());
    writeLoop (fixture.manager, 1, 1, note (64));
    auto groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].passOrdinal == 1);
    REQUIRE (groups[0].tracks.size() == 2);
    REQUIRE (groups[0].tracks[0].trackIndex == 0);
    REQUIRE (groups[0].tracks[1].trackIndex == 1);
    REQUIRE (groups[0].tracks[0].pass.notes[0].noteNumber == 60);
    REQUIRE (groups[0].tracks[1].pass.notes[0].noteNumber == 64);
    dusk::MidiBuffer empty;
    writeLoop (fixture.manager, 1, 2, empty);
    groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].passOrdinal == 2);
    REQUIRE (groups[0].tracks.size() == 1);
    REQUIRE (groups[0].tracks[0].pass.notes[0].noteNumber == 62);
    writeLoop (fixture.manager, 0, 3, empty);
    writeLoop (fixture.manager, 1, 3, empty);
    groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].passOrdinal == 3);
    REQUIRE (groups[0].tracks.empty());
    fixture.manager.stopRecording (200);
    REQUIRE (fixture.manager.takeMidiPassGroups().empty());
}

TEST_CASE ("RecordManager final MIDI merge group survives teardown and seals a partial loop once",
           "[recording][recordmanager][midi][merge]")
{
    CaptureFixture fixture (true);
    REQUIRE (fixture.manager.startRecording (kSampleRate, 100, 0, loopPlan()));
    writeLoop (fixture.manager, 0, 1, note (60));
    REQUIRE (fixture.manager.takeMidiPassGroups().size() == 1);
    dusk::MidiBuffer held;
    addMessage (held, 0x90, 64, 90, 30);
    writeLoop (fixture.manager, 0, 2, held, 40);
    REQUIRE (fixture.manager.takeMidiPassGroups().empty());
    fixture.manager.stopRecording (140);
    const auto groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].passOrdinal == 2);
    REQUIRE (groups[0].tracks.size() == 1);
    REQUIRE (groups[0].tracks[0].pass.lengthInSamples == 40);
    REQUIRE (groups[0].tracks[0].pass.notes == std::vector<MidiNote> { { 1, 64, 90, 30, 10 } });
    REQUIRE (fixture.manager.getLastCommitDiff().empty());
    REQUIRE (fixture.session.track (0).midiRegions.current().empty());
    fixture.manager.stopRecording (140);
    REQUIRE (fixture.manager.takeMidiPassGroups().empty());
}

TEST_CASE ("RecordManager stopping at a MIDI loop seam cannot create a phantom held-note pass",
           "[recording][recordmanager][midi][merge]")
{
    CaptureFixture fixture (true);
    REQUIRE (fixture.manager.startRecording (kSampleRate, 100, 0, loopPlan()));
    dusk::MidiBuffer held;
    addMessage (held, 0x90, 60, 100, 70);
    writeLoop (fixture.manager, 0, 1, held);
    const auto groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].tracks[0].pass.notes == std::vector<MidiNote> { { 1, 60, 100, 70, 30 } });
    fixture.manager.beginLoopCaptureSpan ({});
    fixture.manager.stopRecording (100);
    REQUIRE (fixture.manager.takeMidiPassGroups().empty());
}

TEST_CASE ("RecordManager MIDI merge uses the captured tempo after a mid-gesture tempo change",
           "[recording][recordmanager][midi][merge]")
{
    CaptureFixture fixture (true);
    REQUIRE (fixture.manager.startRecording (kSampleRate, 1000));
    fixture.session.tempoBpm.store (60.0f, std::memory_order_relaxed);
    fixture.manager.writeMidiBlock (0, note (60), 0, nullptr, 100);
    fixture.manager.stopRecording (1100);
    const auto groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE_THAT (groups[0].capturedSampleRate, WithinAbs (kSampleRate, 1.0e-9));
    REQUIRE_THAT ((double) groups[0].capturedBpm, WithinAbs (120.0, 1.0e-9));
    REQUIRE (groups[0].tracks[0].pass.lengthInTicks == 100);
    REQUIRE (groups[0].tracks[0].pass.notes == std::vector<MidiNote> { { 1, 60, 100, 10, 20 } });
}

TEST_CASE ("RecordManager Merge OFF retains all tagged channel-control messages",
           "[recording][recordmanager][midi][merge]")
{
    CaptureFixture fixture (false);
    SECTION ("All tagged controls retain their captured values and positions")
    {
        REQUIRE (fixture.manager.startRecording (kSampleRate, 0));
        fixture.manager.writeMidiBlock (0, controls(), 0, nullptr, 100);
        fixture.manager.serviceMidiCapture();
        REQUIRE (fixture.manager.midiCapturePreview (0)->ccs.size() == 5);
        REQUIRE (fixture.manager.takeMidiPassGroups().empty());
        fixture.manager.stopRecording (100);
        const auto regions = fixture.session.track (0).midiRegions.current();
        REQUIRE (regions.size() == 1);
        REQUIRE (regions[0].ccs == std::vector<MidiCc> {
            { 2, 64, 127, 1, MidiCc::Kind::ControlChange },
            { 3, 0, 8193, 2, MidiCc::Kind::PitchBend },
            { 4, 0, 70, 3, MidiCc::Kind::ChannelPressure },
            { 5, 60, 80, 4, MidiCc::Kind::PolyPressure },
            { 6, 0, 9, 5, MidiCc::Kind::ProgramChange } });
    }
    SECTION ("Final retained samples stay inside the half-open tick range")
    {
        bool looping = false;
        int sampleLength = 50;
        SECTION ("Straight capture") {}
        SECTION ("Loop capture") { looping = true; }
        SECTION ("Straight capture shorter than one tick") { sampleLength = 1; }
        SECTION ("Loop capture shorter than one tick") { looping = true; sampleLength = 1; }
        const RecordManager::LoopCapturePlan plan { looping, 0, sampleLength, 0, sampleLength };
        REQUIRE (fixture.manager.startRecording (9600.0, 0, 0, plan));
        dusk::MidiBuffer buffer;
        const int lastSample = sampleLength - 1;
        addMessage (buffer, 0x90, 60, 100, std::max (0, sampleLength - 2));
        addMessage (buffer, 0x80, 60, 0, lastSample);
        addMessage (buffer, 0x90, 64, 90, lastSample);
        addMessage (buffer, 0xb1, 64, 127, lastSample);
        addMessage (buffer, 0xe2, 1, 64, lastSample);
        addMessage (buffer, 0xd3, 70, 0, lastSample);
        addMessage (buffer, 0xa4, 60, 80, lastSample);
        addMessage (buffer, 0xc5, 9, 0, lastSample);
        addMessage (buffer, 0xb0, 7, 60, sampleLength);
        addMessage (buffer, 0x80, 64, 0, sampleLength);
        addMessage (buffer, 0x90, 67, 80, sampleLength);
        if (looping)
        {
            const auto span = fixture.manager.coordinateLoopCaptureSpan (1, 0, 0, sampleLength + 10);
            fixture.manager.beginLoopCaptureSpan (span);
            fixture.manager.writeMidiBlock (0, buffer, 0, &span, sampleLength + 10);
        }
        else
            fixture.manager.writeMidiBlock (0, buffer, 0, nullptr, sampleLength + 10);
        fixture.manager.stopRecording (sampleLength);
        const auto regions = fixture.session.track (0).midiRegions.current();
        REQUIRE (regions.size() == 1);
        const auto& region = regions[0];
        const std::int64_t tickLength = sampleLength == 1 ? 1 : 5;
        REQUIRE (region.lengthInSamples == sampleLength);
        REQUIRE (region.lengthInTicks == tickLength);
        REQUIRE (region.notes == std::vector<MidiNote> {
            { 1, 60, 100, tickLength - 1, 1 }, { 1, 64, 90, tickLength - 1, 1 } });
        REQUIRE (region.ccs == std::vector<MidiCc> {
            { 2, 64, 127, tickLength - 1, MidiCc::Kind::ControlChange },
            { 3, 0, 8193, tickLength - 1, MidiCc::Kind::PitchBend },
            { 4, 0, 70, tickLength - 1, MidiCc::Kind::ChannelPressure },
            { 5, 60, 80, tickLength - 1, MidiCc::Kind::PolyPressure },
            { 6, 0, 9, tickLength - 1, MidiCc::Kind::ProgramChange } });
    }
    SECTION ("A stop at the capture start excludes every pending event")
    {
        REQUIRE (fixture.manager.startRecording (9600.0, 0));
        dusk::MidiBuffer buffer;
        addMessage (buffer, 0xb0, 7, 100, 0);
        addMessage (buffer, 0x90, 60, 100, 0);
        fixture.manager.writeMidiBlock (0, buffer, 0, nullptr, 10);
        fixture.manager.stopRecording (0);
        REQUIRE (fixture.session.track (0).midiRegions.current().empty());
        REQUIRE (fixture.manager.getLastCommitDiff().empty());
    }
}

TEST_CASE ("RecordManager Merge OFF loop takes chase every tagged control but reset only CCs",
           "[recording][recordmanager][midi][merge]")
{
    CaptureFixture fixture (false);
    REQUIRE (fixture.manager.startRecording (kSampleRate, 100, 0, loopPlan()));
    writeLoop (fixture.manager, 0, 1, controls());
    dusk::MidiBuffer empty;
    writeLoop (fixture.manager, 0, 2, empty);
    fixture.manager.stopRecording (200);
    const auto regions = fixture.session.track (0).midiRegions.current();
    REQUIRE (regions.size() == 1);
    REQUIRE (regions[0].ccs.size() == 5);
    for (const auto& control : regions[0].ccs)
    {
        REQUIRE (control.atTick == 0);
        REQUIRE (control.value > 0);
    }
    REQUIRE (regions[0].previousTakes.size() == 1);
    const auto& previous = regions[0].previousTakes[0].ccs;
    REQUIRE (previous.size() == 6);
    REQUIRE (std::count_if (previous.begin(), previous.end(), [] (const MidiCc& control)
        { return control.value == 0; }) == 1);
    REQUIRE (previous.back().kind == MidiCc::Kind::ControlChange);
    REQUIRE (previous.back().controller == 64);
    REQUIRE (previous.back().atTick == 99);
}

TEST_CASE ("RecordManager MIDI merge reports stream overflow without legacy recording commits",
           "[recording][recordmanager][midi][merge]")
{
    CaptureFixture fixture (true);
    REQUIRE (fixture.manager.startRecording (kSampleRate, 0));
    dusk::MidiBuffer buffer;
    constexpr int kCount = (int) MidiCaptureStream::kDefaultCapacity + 4;
    for (int index = 0; index < kCount; ++index)
        addMessage (buffer, 0xb0, 1, (std::uint8_t) (index & 127), index);
    fixture.manager.writeMidiBlock (0, buffer, 0, nullptr, kCount);
    fixture.manager.stopRecording (kCount);
    const auto& errors = fixture.manager.getLastRecordErrors();
    REQUIRE (errors.size() == 1);
    REQUIRE (errors[0].kind == RecordManager::RecordErrorKind::MidiOverflow);
    REQUIRE (errors[0].count == 5);
    const auto groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].tracks[0].pass.ccs.size() == MidiCaptureStream::kDefaultCapacity);
    REQUIRE (groups[0].tracks[0].pass.ccs.front().atTick == 0);
    REQUIRE (groups[0].tracks[0].pass.ccs.back().atTick == (int) MidiCaptureStream::kDefaultCapacity - 1);
    REQUIRE (fixture.session.track (0).midiRegions.current().empty());
    REQUIRE (fixture.manager.getLastCommitDiff().empty());
}

TEST_CASE ("RecordManager MIDI merge explicit spans retain only the captured callback slice",
           "[recording][recordmanager][midi][merge]")
{
    CaptureFixture fixture (true);
    RecordManager::LoopCapturePlan plan { true, 100, 300, 150, 250 };
    REQUIRE (fixture.manager.startRecording (kSampleRate, 100, 0, plan));
    const auto span = fixture.manager.coordinateLoopCaptureSpan (1, 100, 7, 200);
    REQUIRE (span.inputOffset == 57);
    REQUIRE (span.numSamples == 100);
    dusk::MidiBuffer buffer;
    addMessage (buffer, 0x90, 60, 100, 50);
    addMessage (buffer, 0x90, 64, 90, 60);
    addMessage (buffer, 0x80, 64, 0, 80);
    addMessage (buffer, 0x90, 67, 80, 160);
    fixture.manager.writeMidiBlock (0, buffer, 0, &span, 200);
    const auto groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].timelineStart == 150);
    REQUIRE (groups[0].tracks[0].pass.notes == std::vector<MidiNote> { { 1, 64, 90, 3, 20 } });
    fixture.manager.stopRecording (250);
    REQUIRE (fixture.manager.takeMidiPassGroups().empty());
}

TEST_CASE ("RecordManager MIDI merge coordination excludes armed tracks without a capture",
           "[recording][recordmanager][midi][merge]")
{
    CaptureFixture fixture (true, 2);
    fixture.session.track (1).frozen.store (true, std::memory_order_relaxed);
    REQUIRE (fixture.manager.startRecording (kSampleRate, 100, 0, loopPlan()));
    REQUIRE (fixture.manager.getActiveMidiCaptureTrackMask() == 1);
    writeLoop (fixture.manager, 0, 1, note (60));
    const auto groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].tracks.size() == 1);
    REQUIRE (groups[0].tracks[0].trackIndex == 0);
    fixture.manager.stopRecording (200);
    REQUIRE (fixture.manager.takeMidiPassGroups().empty());
}

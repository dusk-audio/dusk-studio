#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/RecordManager.h"
#include "engine/MidiTimelineSchedule.h"
#include "session/Session.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
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
    fixture.session.tempoBpm.store (60.0f);
    auto prepared = fixture.manager.prepareMidiCapture (false);
    fixture.session.tempoBpm.store (120.0f);
    REQUIRE_FALSE (fixture.manager.isActive());
    REQUIRE (fixture.manager.getActiveCaptureTrackMask() == 0);
    REQUIRE (fixture.session.track (0).midiRegions.current()[0].notes == existing.notes);
    fixture.session.midiMergeEnabled.store (! merge);
    REQUIRE_FALSE (fixture.manager.startRecording (kSampleRate, 1000, 0, {}, std::move (prepared)));
    fixture.session.midiMergeEnabled.store (merge);
    fixture.session.track (1).mode.store ((int) Track::Mode::Midi);
    fixture.session.setTrackArmed (1, true);
    REQUIRE_FALSE (fixture.manager.startRecording (kSampleRate, 1000, 0, {}, std::move (prepared)));
    fixture.session.setTrackArmed (1, false);
    REQUIRE_FALSE (fixture.manager.startRecording (kSampleRate, 1000, 0, loopPlan(), std::move (prepared)));
    REQUIRE_FALSE (fixture.manager.isActive());
    REQUIRE (fixture.manager.startRecording (kSampleRate, 1000, 0, {}, std::move (prepared)));
    fixture.session.midiMergeEnabled.store (! merge, std::memory_order_relaxed);
    REQUIRE (fixture.manager.isMidiMergeCapture() == merge);
    REQUIRE (fixture.manager.midiCaptureTimelineStart() == 1000);
    fixture.manager.withPendingMidiPlayback (0, [] (auto* pending)
    { CHECK (pending == nullptr); });
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

TEST_CASE ("RecordManager linear MIDI capture clips preview and commits at punch-out",
           "[recording][recordmanager][midi][merge][punch]")
{
    const bool merge = GENERATE (false, true);
    const std::int64_t captureEnd = GENERATE (200, 100);
    CAPTURE (merge, captureEnd);
    CaptureFixture fixture (merge);
    auto prepared = fixture.manager.prepareMidiCapture (false);
    REQUIRE (fixture.manager.startRecording (kSampleRate, 100, 0, {}, std::move (prepared), captureEnd));

    dusk::MidiBuffer first;
    addMessage (first, 0x90, 60, 100, 10);
    addMessage (first, 0x90, 64, 100, 20);
    addMessage (first, 0xb0, 1, 30, 30);
    fixture.manager.writeMidiBlock (0, first, -20, nullptr, 70);
    dusk::MidiBuffer crossing;
    addMessage (crossing, 0xb0, 1, 40, 49);
    addMessage (crossing, 0x90, 67, 100, 50);
    addMessage (crossing, 0xb0, 1, 50, 50);
    addMessage (crossing, 0x90, 69, 100, 70);
    addMessage (crossing, 0xb0, 1, 60, 70);
    addMessage (crossing, 0x80, 64, 0, 80);
    fixture.manager.writeMidiBlock (0, crossing, 50, nullptr, 150);
    fixture.manager.serviceMidiCapture();

    const auto length = captureEnd - 100;
    const std::vector<MidiNote> notes = length > 0
        ? std::vector<MidiNote> { { 1, 64, 100, 0, length } } : std::vector<MidiNote> {};
    const std::vector<MidiCc> ccs = length > 0
        ? std::vector<MidiCc> { { 1, 1, 30, 10 }, { 1, 1, 40, 99 } } : std::vector<MidiCc> {};
    const auto* preview = fixture.manager.midiCapturePreview (0);
    REQUIRE (preview != nullptr);
    CHECK (preview->lengthInSamples == length);
    CHECK (preview->notes == notes);
    CHECK (preview->ccs == ccs);

    fixture.manager.stopRecording (400);
    const auto groups = fixture.manager.takeMidiPassGroups();
    const auto& regions = fixture.session.track (0).midiRegions.current();
    if (merge)
    {
        REQUIRE (groups.size() == 1);
        REQUIRE (groups[0].tracks.size() == (length > 0 ? 1 : 0));
        REQUIRE (regions.empty());
        if (length > 0)
        {
            CHECK (groups[0].tracks[0].pass.lengthInSamples == length);
            CHECK (groups[0].tracks[0].pass.notes == notes);
            CHECK (groups[0].tracks[0].pass.ccs == ccs);
        }
    }
    else
    {
        REQUIRE (groups.empty());
        REQUIRE (regions.size() == (length > 0 ? 1 : 0));
        if (length > 0)
        {
            CHECK (regions[0].lengthInSamples == length);
            CHECK (regions[0].notes == notes);
            CHECK (regions[0].ccs == ccs);
        }
    }
}

TEST_CASE ("RecordManager linear MIDI capture follows punch changes during recording",
           "[recording][recordmanager][midi][merge][punch]")
{
    const bool merge = GENERATE (false, true);
    const std::int64_t editedEnd = GENERATE (150, 300, -1);
    CAPTURE (merge, editedEnd);
    CaptureFixture fixture (merge);
    auto prepared = fixture.manager.prepareMidiCapture (false);
    REQUIRE (fixture.manager.startRecording (kSampleRate, 100, 0, {}, std::move (prepared), 200));
    fixture.manager.writeMidiBlock (0, note (60), 0, nullptr, 150);
    fixture.manager.serviceMidiCapture();
    fixture.manager.setLinearMidiCaptureEndSample (editedEnd >= 0
        ? std::optional<std::int64_t> (editedEnd) : std::nullopt);
    fixture.manager.writeMidiBlock (0, note (64, 60, 90), 50, nullptr, 200);
    fixture.manager.serviceMidiCapture();
    const auto length = editedEnd >= 0 ? editedEnd - 100 : 250;
    std::vector<MidiNote> expected { { 1, 60, 100, 10, 20 } };
    if (length > 110) expected.push_back ({ 1, 64, 100, 110, 30 });
    const auto* preview = fixture.manager.midiCapturePreview (0);
    REQUIRE (preview != nullptr);
    CHECK (preview->lengthInSamples == length);
    CHECK (preview->notes == expected);
    fixture.manager.stopRecording (350);
    const auto groups = fixture.manager.takeMidiPassGroups();
    const auto& regions = fixture.session.track (0).midiRegions.current();
    if (merge)
    {
        REQUIRE (groups.size() == 1);
        REQUIRE (groups[0].tracks.size() == 1);
        CHECK (groups[0].tracks[0].pass.lengthInSamples == length);
        CHECK (groups[0].tracks[0].pass.notes == expected);
        CHECK (regions.empty());
    }
    else
    {
        REQUIRE (regions.size() == 1);
        CHECK (regions[0].lengthInSamples == length);
        CHECK (regions[0].notes == expected);
        CHECK (groups.empty());
    }
}

TEST_CASE ("RecordManager preserves punch-out note endings when linear capture resumes",
           "[recording][recordmanager][midi][merge][punch]")
{
    const bool merge = GENERATE (false, true);
    const std::uint8_t releaseStatus = GENERATE (std::uint8_t { 0x80 }, std::uint8_t { 0x90 });
    CaptureFixture fixture (merge);
    auto prepared = fixture.manager.prepareMidiCapture (false);
    REQUIRE (fixture.manager.startRecording (kSampleRate, 100, 0, {}, std::move (prepared), 200));
    dusk::MidiBuffer held;
    addMessage (held, 0x90, 60, 100, 10);
    addMessage (held, releaseStatus, 60, 0, 140);
    fixture.manager.writeMidiBlock (0, held, 0, nullptr, 200);
    fixture.manager.serviceMidiCapture();
    const std::vector<MidiNote> expected { { 1, 60, 100, 10, 90 } };
    REQUIRE (fixture.manager.midiCapturePreview (0)->notes == expected);
    fixture.manager.setLinearMidiCaptureEndSample (std::nullopt);
    fixture.manager.writeMidiBlock (0, {}, 200, nullptr, 50);
    fixture.manager.serviceMidiCapture();
    CHECK (fixture.manager.midiCapturePreview (0)->notes == expected);
    fixture.manager.stopRecording (350);
    if (merge)
    {
        const auto groups = fixture.manager.takeMidiPassGroups();
        REQUIRE (groups.size() == 1);
        REQUIRE (groups[0].tracks.size() == 1);
        CHECK (groups[0].tracks[0].pass.notes == expected);
    }
    else
    {
        const auto& regions = fixture.session.track (0).midiRegions.current();
        REQUIRE (regions.size() == 1);
        CHECK (regions[0].notes == expected);
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
    const auto gesture = groups[0].gestureId;
    REQUIRE (gesture != 0);
    REQUIRE (groups[0].tracks.size() == 2);
    REQUIRE (groups[0].tracks[0].trackIndex == 0);
    REQUIRE (groups[0].tracks[1].trackIndex == 1);
    REQUIRE (groups[0].tracks[0].pass.notes[0].noteNumber == 60);
    REQUIRE (groups[0].tracks[1].pass.notes[0].noteNumber == 64);
    fixture.manager.withPendingMidiPlayback (0, [gesture] (auto* pending)
    {
        REQUIRE (pending != nullptr);
        std::vector<MidiNote> notes;
        int scans = 100;
        REQUIRE (pending->forEachNote ({}, 3, scans, [&notes] (const auto& note)
        { notes.push_back (note); return true; }));
        CHECK (notes == std::vector<MidiNote> { { 1, 60, 100, 10, 20 }, { 1, 62, 100, 10, 20 } });
        pending->discardConsumed ({ gesture, 1 });
        notes.clear();
        REQUIRE (pending->forEachNote ({ gesture, 1 }, 3, scans, [&notes] (const auto& note)
        { notes.push_back (note); return true; }));
        CHECK (notes == std::vector<MidiNote> { { 1, 62, 100, 10, 20 } });
    });
    dusk::MidiBuffer empty;
    writeLoop (fixture.manager, 1, 2, empty);
    groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].passOrdinal == 2);
    REQUIRE (groups[0].gestureId == gesture);
    REQUIRE (groups[0].tracks.size() == 1);
    REQUIRE (groups[0].tracks[0].pass.notes[0].noteNumber == 62);
    writeLoop (fixture.manager, 0, 3, empty);
    writeLoop (fixture.manager, 1, 3, empty);
    groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].passOrdinal == 3);
    REQUIRE (groups[0].gestureId == gesture);
    REQUIRE (groups[0].tracks.empty());
    fixture.manager.stopRecording (200);
    REQUIRE (fixture.manager.takeMidiPassGroups().empty());
    bool stoppedVisited = false;
    fixture.manager.withPendingMidiPlayback (0, [&stoppedVisited] (auto* pending)
    {
        stoppedVisited = true;
        CHECK (pending == nullptr);
    });
    CHECK (stoppedVisited);
    REQUIRE (fixture.manager.startRecording (kSampleRate, 100, 0, loopPlan()));
    writeLoop (fixture.manager, 0, 1, note (67));
    writeLoop (fixture.manager, 1, 1, empty);
    groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].gestureId != 0);
    REQUIRE (groups[0].gestureId != gesture);
    fixture.manager.stopRecording (200);
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
    int extra = 0;
    bool looping = false;
    SECTION ("Only the progress marker overflows") {}
    SECTION ("Four channel messages overflow") { extra = 4; }
    SECTION ("A dropped loop marker preserves pending playback") { looping = true; }
    const int count = (int) MidiCaptureStream::kDefaultCapacity + extra;
    RecordManager::LoopCapturePlan plan;
    if (looping) plan = { true, 0, count, 0, count };
    REQUIRE (fixture.manager.startRecording (kSampleRate, 0, 0, plan));
    dusk::MidiBuffer buffer;
    addMessage (buffer, 0x90, 60, 100, 0);
    if (looping) addMessage (buffer, 0x80, 60, 0, 1);
    for (int index = looping ? 2 : 1; index < count; ++index)
        addMessage (buffer, 0xb0, 1, (std::uint8_t) (index & 127), index);
    if (looping)
    {
        const auto span = fixture.manager.coordinateLoopCaptureSpan (1, 0, 0, count);
        fixture.manager.beginLoopCaptureSpan (span);
        fixture.manager.writeMidiBlock (0, buffer, 0, &span, count);
        bool present = false, overflow = false, eligible = false, iterated = false, notesMatch = true;
        int notes = 0;
        fixture.manager.withPendingMidiPlayback (0, [&] (auto* pending)
        {
            if (pending == nullptr) return;
            present = true;
            overflow = pending->takeOverflow();
            int scans = (int) MidiCaptureStream::kDefaultCapacity + 1;
            eligible = pending->hasEligible ({}, 2, scans);
            scans = (int) MidiCaptureStream::kDefaultCapacity + 1;
            iterated = pending->forEachNote ({}, 2, scans, [&] (const MidiNote& note)
            {
                ++notes;
                notesMatch = notesMatch && note == MidiNote { 1, 60, 100, 0, 1 };
                return true;
            });
        });
        REQUIRE (present);
        REQUIRE_FALSE (overflow);
        REQUIRE (eligible);
        REQUIRE (iterated);
        REQUIRE (notes == 1);
        REQUIRE (notesMatch);
    }
    else
        fixture.manager.writeMidiBlock (0, buffer, 0, nullptr, count);
    fixture.manager.stopRecording (count);
    const auto& errors = fixture.manager.getLastRecordErrors();
    if (extra == 0) REQUIRE (errors.empty());
    else
    {
        REQUIRE (errors.size() == 1);
        REQUIRE (errors[0].kind == RecordManager::RecordErrorKind::MidiOverflow);
        REQUIRE (errors[0].count == (std::uint64_t) extra);
    }
    const auto groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].tracks[0].pass.ccs.size() == MidiCaptureStream::kDefaultCapacity - (looping ? 2 : 1));
    REQUIRE (groups[0].tracks[0].pass.ccs.front().atTick == (looping ? 2 : 1));
    REQUIRE (groups[0].tracks[0].pass.ccs.back().atTick == (int) MidiCaptureStream::kDefaultCapacity - 1);
    if (extra == 0)
        REQUIRE (groups[0].tracks[0].pass.notes == std::vector<MidiNote> { { 1, 60, 100, 0, looping ? 1 : count } });
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

TEST_CASE ("MIDI capture uses the tempo map at its timeline origin across tempo changes",
           "[recording][recordmanager][midi][merge][tempo]")
{
    const bool merge = GENERATE (false, true);
    const bool loop = GENERATE (false, true);
    CAPTURE (merge, loop);
    CaptureFixture fixture (merge);
    fixture.session.tempoMap.setPoints ({ { 0, 120.0f }, { 48000, 60.0f } });
    constexpr double rate = 48000.0;
    constexpr std::int64_t start = 36000, length = 48000;
    const RecordManager::LoopCapturePlan plan = loop
        ? RecordManager::LoopCapturePlan { true, start, start + length, start, start + length }
        : RecordManager::LoopCapturePlan {};
    REQUIRE (fixture.manager.startRecording (rate, start, 0, plan));
    auto buffer = note (60, 6000, 24000);
    addMessage (buffer, 0xb0, 7, 90, 18000);
    auto span = fixture.manager.coordinateLoopCaptureSpan (1, start, 0, (int) length);
    if (loop) fixture.manager.beginLoopCaptureSpan (span);
    fixture.manager.writeMidiBlock (0, buffer, 0, loop ? &span : nullptr, (int) length);
    fixture.manager.serviceMidiCapture();
    const auto* preview = fixture.manager.midiCapturePreview (0);
    REQUIRE (preview != nullptr);
    if (! loop)
    {
        CHECK (preview->lengthInTicks == 600);
        CHECK (preview->notes == std::vector<MidiNote> { { 1, 60, 100, 120, 240 } });
        CHECK (preview->ccs == std::vector<MidiCc> { { 1, 7, 90, 300 } });
    }
    if (merge && loop)
        fixture.manager.withPendingMidiPlayback (0, [&] (auto* pending)
        {
            REQUIRE (pending != nullptr);
            std::vector<MidiNote> notes;
            std::vector<MidiCc> ccs;
            int scans = 32768;
            REQUIRE (pending->forEachNote ({}, 2, scans, [&] (const auto& n)
            { notes.push_back (n); return true; }));
            REQUIRE (pending->forEachControl ({}, 2, scans, [&] (const auto& c)
            { ccs.push_back (c); return true; }));
            CHECK (notes == std::vector<MidiNote> { { 1, 60, 100, 120, 240 } });
            CHECK (ccs == std::vector<MidiCc> { { 1, 7, 90, 300 } });
        });
    fixture.manager.stopRecording (start + length);
    if (merge)
    {
        const auto groups = fixture.manager.takeMidiPassGroups();
        REQUIRE (groups.size() == 1);
        REQUIRE (groups[0].tracks.size() == 1);
        CHECK (groups[0].tracks[0].pass.lengthInTicks == 600);
        CHECK (groups[0].tracks[0].pass.notes == std::vector<MidiNote> { { 1, 60, 100, 120, 240 } });
        CHECK (groups[0].tracks[0].pass.ccs == std::vector<MidiCc> { { 1, 7, 90, 300 } });
    }
    else
    {
        const auto& regions = fixture.session.track (0).midiRegions.current();
        REQUIRE (regions.size() == 1);
        CHECK (regions[0].lengthInTicks == 600);
        CHECK (regions[0].notes == std::vector<MidiNote> { { 1, 60, 100, 120, 240 } });
        CHECK (regions[0].ccs == std::vector<MidiCc> { { 1, 7, 90, 300 } });
    }
}

TEST_CASE ("MIDI capture retains its immutable tempo clock when the session map is edited",
           "[recording][recordmanager][midi][merge][tempo]")
{
    CaptureFixture fixture (true);
    fixture.session.tempoMap.setPoints ({ { 0, 120.0f }, { 48000, 60.0f } });
    REQUIRE (fixture.manager.startRecording (48000.0, 48000));
    fixture.session.tempoMap.setPoints ({ { 0, 180.0f } });
    fixture.manager.writeMidiBlock (0, note (60, 12000, 24000), 0, nullptr, 48000);
    fixture.manager.stopRecording (96000);
    const auto groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].tracks.size() == 1);
    CHECK (groups[0].tracks[0].pass.lengthInTicks == 480);
    CHECK (groups[0].tracks[0].pass.notes == std::vector<MidiNote> { { 1, 60, 100, 120, 120 } });
}

TEST_CASE ("Pending MIDI Merge uses the current playback clock after a constant-tempo edit",
           "[recording][recordmanager][midi][merge][tempo]")
{
    CaptureFixture fixture (true);
    const RecordManager::LoopCapturePlan plan { true, 24000, 48000, 24000, 48000 };
    REQUIRE (fixture.manager.startRecording (48000.0, 24000, 0, plan));
    const auto span = fixture.manager.coordinateLoopCaptureSpan (1, 24000, 0, 24000);
    fixture.manager.beginLoopCaptureSpan (span);
    fixture.manager.writeMidiBlock (0, note (60, 0, 12000), 0, &span, 24000);
    fixture.session.tempoBpm.store (60.0f);
    MidiRegion target;
    target.lengthInTicks = 1920;
    target.lengthInSamples = 192000;
    const auto timeline = buildMidiTimeline ({ target });
    const midischedule::RegionClock clock (nullptr, 48000.0, 60.0f, 0);
    fixture.manager.withPendingMidiPlayback (0, [&] (auto* pending)
    {
        REQUIRE (pending != nullptr);
        midischedule::PendingCapture overlay;
        overlay.playback = pending;
        overlay.playbackPass = 2;
        int scans = 32768;
        REQUIRE (overlay.locateTarget (*timeline, 24000, 48000, 48000.0,
                                      fixture.session.tempoBpm.load(), scans));
        CHECK (overlay.offsetTicks == 240);
        const auto outcome = overlay.notes (scans, [&] (const MidiNote& n)
        {
            CHECK (clock.at (n.startTick) == 24000);
            CHECK (clock.at (n.startTick + n.lengthInTicks) == 48000);
            return midischedule::Outcome::complete;
        });
        REQUIRE (outcome == midischedule::Outcome::complete);
    });
    fixture.manager.stopRecording (48000);
    const auto groups = fixture.manager.takeMidiPassGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups[0].tracks.size() == 1);
    MidiRegion pass;
    pass.lengthInTicks = groups[0].tracks[0].pass.lengthInTicks;
    pass.notes = groups[0].tracks[0].pass.notes;
    REQUIRE (mergeMidiPassAt (target, pass, groups[0].timelineStart, clock).changed);
    REQUIRE (target.notes.size() == 1);
    CHECK (clock.at (target.notes[0].startTick) == 24000);
    CHECK (clock.at (target.notes[0].startTick + target.notes[0].lengthInTicks) == 48000);
}

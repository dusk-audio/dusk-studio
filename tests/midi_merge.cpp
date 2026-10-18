#include <catch2/catch_test_macros.hpp>

#include "session/MidiMerge.h"
#include "session/Session.h"

using namespace duskstudio;

TEST_CASE ("MIDI merge rebases a pass and preserves the existing performance",
           "[midi][merge]")
{
    MidiRegion target;
    target.lengthInTicks = 960;
    target.notes.push_back ({ 1, 36, 90, 0, 120 });
    target.ccs.push_back ({ 1, 64, 127, 240 });
    target.label = "Drums";
    target.previousTakes.push_back ({});

    MidiRegion pass;
    pass.lengthInTicks = 960;
    pass.notes.push_back ({ 2, 38, 100, 120, 60 });
    pass.ccs.push_back ({ 2, 7, 80, 0 });

    const auto merged = mergeMidiPass (target, pass, 480);
    REQUIRE (merged.changed);
    REQUIRE (merged.leadingTicks == 0);
    REQUIRE (target.lengthInTicks == 1440);
    REQUIRE (target.notes.size() == 2);
    REQUIRE (target.notes[0] == (MidiNote { 1, 36, 90, 0, 120 }));
    REQUIRE (target.notes[1] == (MidiNote { 2, 38, 100, 600, 60 }));
    REQUIRE (target.ccs[0] == (MidiCc { 1, 64, 127, 240 }));
    REQUIRE (target.ccs[1] == (MidiCc { 2, 7, 80, 480 }));
    REQUIRE (target.label == "Drums");
    REQUIRE (target.previousTakes.size() == 1);
}

TEST_CASE ("MIDI merge extends left without moving the original musical performance",
           "[midi][merge]")
{
    MidiRegion target;
    target.lengthInTicks = 960;
    target.notes.push_back ({ 1, 60, 100, 120, 240 });
    target.ccs.push_back ({ 1, 11, 90, 60 });
    MidiRegion pass;
    pass.lengthInTicks = 480;
    pass.notes.push_back ({ 1, 64, 80, 0, 120 });

    const auto merged = mergeMidiPass (target, pass, -240);
    REQUIRE (merged.changed);
    REQUIRE (merged.leadingTicks == 240);
    REQUIRE (target.lengthInTicks == 1200);
    REQUIRE (target.notes[0].noteNumber == 64);
    REQUIRE (target.notes[0].startTick == 0);
    REQUIRE (target.notes[1].noteNumber == 60);
    REQUIRE (target.notes[1].startTick == 360);
    REQUIRE (target.ccs[0].atTick == 300);
}

TEST_CASE ("MIDI merge growing left preserves previous takes at their absolute ticks", "[midi][merge]")
{
    MidiRegion target;
    target.lengthInTicks = 960;
    target.notes.push_back ({ 1, 60, 100, 120, 240 });
    target.ccs.push_back ({ 1, 11, 90, 60 });
    target.previousTakes.push_back ({ 720, { { 2, 64, 80, 240, 120 } }, { { 2, 7, 90, 300 } }, {} });
    target.previousTakes.push_back ({ 480, { { 3, 67, 75, 0, 60 } }, { { 3, 64, 127, 120 } }, {} });
    MidiRegion pass;
    pass.lengthInTicks = 480;
    pass.notes.push_back ({ 1, 36, 100, 0, 120 });
    const auto before = target;
    const auto merged = mergeMidiPass (target, pass, -480);
    REQUIRE (merged.changed);
    REQUIRE (merged.leadingTicks == 480);
    constexpr std::int64_t oldStart = 3840;
    const auto newStart = oldStart - merged.leadingTicks;
    for (const auto& take : before.previousTakes)
    {
        REQUIRE (cycleTake (target, true));
        REQUIRE (target.notes.size() == take.notes.size());
        REQUIRE (target.ccs.size() == take.ccs.size());
        CHECK (newStart + target.notes.front().startTick == oldStart + take.notes.front().startTick);
        CHECK (newStart + target.ccs.front().atTick == oldStart + take.ccs.front().atTick);
        CHECK (newStart + target.lengthInTicks == oldStart + take.lengthInTicks);
    }
    REQUIRE (cycleTake (target, false));
    CHECK (newStart + target.notes.front().startTick == oldStart + before.previousTakes.front().notes.front().startTick);
}

TEST_CASE ("MIDI merge rejects overflowing previous takes before changing any events", "[midi][merge]")
{
    MidiRegion target, pass;
    target.lengthInTicks = 480;
    target.notes.push_back ({ 1, 60, 100, 0, 120 });
    target.previousTakes.push_back ({ 480, { { 2, 64, 80, 0, 120 } }, { { 2, 7, 90, 0 } }, {} });
    auto& take = target.previousTakes.front();
    SECTION ("Length") { take.lengthInTicks = std::numeric_limits<std::int64_t>::max(); }
    SECTION ("Note") { take.notes.front().startTick = std::numeric_limits<std::int64_t>::max(); }
    SECTION ("Controller") { take.ccs.front().atTick = std::numeric_limits<std::int64_t>::max(); }
    const auto before = target;
    pass.notes.push_back ({ 1, 36, 90, 0, 120 });
    REQUIRE_FALSE (mergeMidiPass (target, pass, -480).changed);
    CHECK (target.notes == before.notes);
    CHECK (target.lengthInTicks == before.lengthInTicks);
    CHECK (target.previousTakes.front().notes == before.previousTakes.front().notes);
    CHECK (target.previousTakes.front().ccs == before.previousTakes.front().ccs);
    CHECK (target.previousTakes.front().lengthInTicks == before.previousTakes.front().lengthInTicks);
}

TEST_CASE ("MIDI merge retains repeated pitches and stable equal-tick controller order",
           "[midi][merge]")
{
    MidiRegion target;
    target.lengthInTicks = 480;
    target.notes.push_back ({ 1, 36, 90, 0, 120 });
    target.ccs.push_back ({ 1, 0, 2, 0 });
    MidiRegion pass;
    pass.lengthInTicks = 480;
    pass.notes.push_back ({ 1, 36, 70, 0, 60 });
    pass.ccs.push_back ({ 1, 0, 0, 0, MidiCc::Kind::ProgramChange });

    REQUIRE (mergeMidiPass (target, pass, 0).changed);
    REQUIRE (target.notes.size() == 2);
    REQUIRE (target.notes[0].velocity == 90);
    REQUIRE (target.notes[1].velocity == 70);
    REQUIRE (target.ccs[0].kind == MidiCc::Kind::ControlChange);
    REQUIRE (target.ccs[1].kind == MidiCc::Kind::ProgramChange);
}

TEST_CASE ("An empty MIDI merge pass leaves the region unchanged",
           "[midi][merge]")
{
    MidiRegion target;
    target.lengthInTicks = 480;
    target.notes.push_back ({ 1, 60, 100, 0, 120 });
    MidiRegion pass;
    pass.lengthInTicks = 1920;
    REQUIRE_FALSE (mergeMidiPass (target, pass, 960).changed);
    REQUIRE (target.lengthInTicks == 480);
    REQUIRE (target.notes.size() == 1);
}

TEST_CASE ("MIDI channel controls use distinct chase lanes and correct wire lengths",
           "[midi][controllers]")
{
    MidiCc cc { 3, 7, 100, 0 };
    REQUIRE (cc.laneKey() == 2 * MidiCc::kLanesPerChannel + 7);
    REQUIRE (cc.statusByte() == 0xb2);
    REQUIRE (cc.dataByte1() == 7);
    REQUIRE (cc.dataByte2() == 100);
    REQUIRE (cc.messageSize() == 3);

    MidiCc bend { 3, 0, 8193, 0, MidiCc::Kind::PitchBend };
    REQUIRE (bend.statusByte() == 0xe2);
    REQUIRE (bend.dataByte1() == 1);
    REQUIRE (bend.dataByte2() == 64);
    REQUIRE (bend.laneKey() != cc.laneKey());

    MidiCc pressure { 3, 60, 85, 0, MidiCc::Kind::PolyPressure };
    REQUIRE (pressure.statusByte() == 0xa2);
    REQUIRE (pressure.dataByte1() == 60);
    REQUIRE (pressure.dataByte2() == 85);
    REQUIRE (pressure.laneKey() != bend.laneKey());

    MidiCc channelPressure { 3, 0, 70, 0, MidiCc::Kind::ChannelPressure };
    REQUIRE (channelPressure.statusByte() == 0xd2);
    REQUIRE (channelPressure.dataByte1() == 70);
    REQUIRE (channelPressure.messageSize() == 2);

    MidiCc program { 3, 0, 12, 0, MidiCc::Kind::ProgramChange };
    REQUIRE (program.statusByte() == 0xc2);
    REQUIRE (program.dataByte1() == 12);
    REQUIRE (program.messageSize() == 2);
    REQUIRE (program.laneKey() != channelPressure.laneKey());
}

TEST_CASE ("MIDI merge includes a trailing control inside the region's half-open bounds",
           "[midi][merge]")
{
    MidiRegion target, pass;
    pass.ccs.push_back ({ 1, 0, 7, 480, MidiCc::Kind::ProgramChange });
    REQUIRE (mergeMidiPass (target, pass, 960).changed);
    REQUIRE (target.lengthInTicks == 1441);
    REQUIRE (target.ccs[0].atTick == 1440);

    pass.ccs[0].atTick = std::numeric_limits<std::int64_t>::max();
    const auto before = target.ccs;
    REQUIRE_FALSE (mergeMidiPass (target, pass, 0).changed);
    REQUIRE (target.ccs == before);
    REQUIRE (target.lengthInTicks == 1441);
}

TEST_CASE ("MIDI merge target selection agrees with immutable lock and mute placement",
           "[midi][merge][snapshot]")
{
    std::vector<MidiRegion> regions (3);
    regions[0].timelineStart = 0;
    regions[0].lengthInSamples = 100;
    regions[1].timelineStart = 100;
    regions[1].lengthInSamples = 100;
    regions[1].locked = true;
    regions[2].timelineStart = 110;
    regions[2].lengthInSamples = 100;
    std::int64_t start = 100, end = 110;
    int expected = -1;
    SECTION ("touching boundaries are not overlaps") {}
    SECTION ("an unlocked overlap is selected") { regions[1].locked = false; expected = 1; }
    SECTION ("a muted overlap retains its mute policy")
    { regions[2].timelineStart = 105; regions[2].muted = true; expected = 2; }
    SECTION ("a pass joins the region it starts in, not a later one it runs into")
    { start = 50; end = 150; expected = 0; }
    SECTION ("a pass starting before every overlap joins the earliest of them")
    {
        regions[0].timelineStart = 120;
        regions[2].timelineStart = 130;
        start = 50;
        end = 150;
        expected = 0;
    }
    SECTION ("overlaps starting together after the pass resolve to the later one")
    {
        regions[0].timelineStart = 120;
        regions[2].timelineStart = 120;
        start = 50;
        end = 150;
        expected = 2;
    }
    SECTION ("of stacked regions holding the start, the later one is joined")
    {
        regions[1].locked = false;
        regions[1].timelineStart = 0;
        start = 50;
        end = 150;
        expected = 1;
    }
    SECTION ("a locked region holding the start leaves the earliest unlocked overlap")
    {
        regions[0].timelineStart = 160;
        regions[2].timelineStart = 170;
        start = 150;
        end = 300;
        expected = 0;
    }
    SECTION ("positive endpoints cannot overflow")
    {
        regions[2].timelineStart = std::numeric_limits<std::int64_t>::max() - 10;
        start = regions[2].timelineStart + 5;
        end = std::numeric_limits<std::int64_t>::max();
        expected = 2;
    }
    SECTION ("negative timeline coordinates are retained")
    {
        regions[2].timelineStart = std::numeric_limits<std::int64_t>::min();
        start = regions[2].timelineStart + 10;
        end = start + 20;
        expected = 2;
    }
    const auto timeline = buildMidiTimeline (regions);
    CHECK (findMidiMergeTarget (regions, start, end) == expected);
    CHECK (findMidiMergeTarget (timeline->placement, start, end) == expected);
    CHECK (timeline->placement[1].locked == regions[1].locked);
    CHECK (timeline->placement[2].muted == regions[2].muted);
}

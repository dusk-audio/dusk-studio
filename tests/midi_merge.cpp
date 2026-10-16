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

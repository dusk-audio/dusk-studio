#include <catch2/catch_test_macros.hpp>

#include "engine/MidiCaptureStream.h"
#include "engine/MidiMergePlayback.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace duskstudio;

namespace
{
std::vector<MidiNote> notesFor (const MidiMergePlayback& pending, MidiCaptureStamp consumed,
                               int playbackPass, int scans = 32768)
{
    std::vector<MidiNote> notes;
    REQUIRE (pending.forEachNote (consumed, playbackPass, scans, [&] (const auto& note)
    {
        notes.push_back (note);
        return true;
    }));
    std::stable_sort (notes.begin(), notes.end(), [] (const auto& a, const auto& b)
    { return a.startTick < b.startTick; });
    return notes;
}
}

TEST_CASE ("Pending MIDI Merge notes use the capture stream's completed-pass timing",
           "[midi][recording][merge]")
{
    const auto ticksAt = [] (std::int64_t sample) { return (std::int64_t) std::llround (sample / 50.0); };
    MidiCaptureStream capture;
    MidiMergePlayback pending;
    capture.configure (ticksAt, 384);
    pending.configure (42, 384, ticksAt);
    const auto event = [&] (int at, std::uint8_t status, std::uint8_t key, std::uint8_t velocity)
    {
        REQUIRE (capture.pushEvent (1, at, status, key, velocity));
        pending.pushEvent (1, at, status, key, velocity);
    };
    event (0, 0x90, 60, 90);
    event (180, 0x90, 60, 100);
    event (300, 0x80, 60, 0);
    event (383, 0x9f, 72, 80);
    event (383, 0x8f, 72, 0);
    REQUIRE (capture.publishProgress (1, 384, true));
    pending.publishProgress (1, 384, true);
    capture.service();
    const auto passes = capture.takeCompleted();
    REQUIRE (passes.size() == 1);
    CHECK (notesFor (pending, {}, 2) == passes[0].notes);
    CHECK (notesFor (pending, {}, 1).empty());
    CHECK_FALSE (pending.takeOverflow());
}

TEST_CASE ("Pending MIDI Merge can replay a known onset before its source pass completes",
           "[midi][recording][merge]")
{
    MidiMergePlayback pending;
    pending.configure (12, 512, {});
    pending.pushEvent (1, 0, 0x92, 64, 87);
    pending.publishProgress (1, 256, false);
    int scans = 100;
    CHECK (pending.hasEligible ({}, 2, scans));
    CHECK_FALSE (pending.hasEligible ({}, 1, scans));
    CHECK_FALSE (pending.hasEligible ({ 12, 1 }, 2, scans));
    CHECK (notesFor (pending, {}, 2) == std::vector<MidiNote> { { 3, 64, 87, 0, 512 } });
    pending.pushEvent (1, 300, 0x82, 64, 0);
    CHECK (pending.hasEligible ({}, 2, scans));
    CHECK_FALSE (pending.hasEligible ({ 12, 1 }, 2, scans));
    CHECK (notesFor (pending, {}, 2) == std::vector<MidiNote> { { 3, 64, 87, 0, 300 } });
    CHECK (notesFor (pending, { 12, 1 }, 2).empty());
    CHECK (notesFor (pending, { 11, 999 }, 2).size() == 1);
}

TEST_CASE ("Pending MIDI Merge carries held notes only through captured passes",
           "[midi][recording][merge]")
{
    MidiMergePlayback pending;
    pending.configure (12, 100, {});
    pending.pushEvent (1, 70, 0x9f, 69, 91);
    pending.publishProgress (1, 100, true);
    CHECK (notesFor (pending, {}, 3) == std::vector<MidiNote> { { 16, 69, 91, 70, 30 } });
    pending.publishProgress (2, 10, false);
    CHECK (notesFor (pending, {}, 3) == std::vector<MidiNote> {
        { 16, 69, 91, 0, 100 }, { 16, 69, 91, 70, 30 } });
    pending.publishProgress (2, 100, true);
    pending.pushEvent (3, 25, 0x9f, 69, 0);
    CHECK (notesFor (pending, { 12, 1 }, 4) == std::vector<MidiNote> {
        { 16, 69, 91, 0, 100 }, { 16, 69, 91, 0, 25 } });
    pending.discardConsumed ({ 12, 2 });
    CHECK (notesFor (pending, {}, 4) == std::vector<MidiNote> { { 16, 69, 91, 0, 25 } });
}

TEST_CASE ("Pending MIDI Merge retains channel controls and exact snapshot consumption",
           "[midi][recording][merge]")
{
    MidiMergePlayback pending;
    pending.configure (42, 100, {});
    const std::uint8_t messages[][3] { { 0xb0, 0, 3 }, { 0xe1, 1, 96 }, { 0xd2, 91, 0 },
                                      { 0xa3, 60, 80 }, { 0xc4, 12, 0 } };
    for (const auto& bytes : messages)
        pending.pushEvent (1, 99, bytes[0], bytes[1], bytes[2]);
    pending.publishProgress (1, 100, true);
    std::vector<MidiCc> controls;
    int scans = 100;
    REQUIRE (pending.forEachControl ({}, 2, scans, [&] (const MidiCc& cc)
    { controls.push_back (cc); return true; }));
    REQUIRE (controls.size() == 5);
    for (std::size_t i = 0; i < controls.size(); ++i)
    {
        CHECK (controls[i].statusByte() == messages[i][0]);
        CHECK (controls[i].dataByte1() == messages[i][1]);
        if (controls[i].messageSize() == 3) CHECK (controls[i].dataByte2() == messages[i][2]);
        CHECK (controls[i].atTick == 99);
    }
    pending.discardConsumed ({ 41, 99 });
    scans = 100;
    int count = 0;
    REQUIRE (pending.forEachControl ({ 42, 1 }, 2, scans, [&] (const auto&) { ++count; return true; }));
    CHECK (count == 0);
    pending.discardConsumed ({ 42, 1 });
    scans = 100;
    REQUIRE (pending.forEachControl ({}, 2, scans, [&] (const auto&) { ++count; return true; }));
    CHECK (count == 0);
}

TEST_CASE ("Pending MIDI Merge storage and visits have bounded overload recovery",
           "[midi][recording][merge]")
{
    MidiMergePlayback pending (2);
    pending.configure (42, 100, {});
    pending.pushEvent (1, 1, 0x90, 60, 100);
    pending.pushEvent (1, 2, 0x90, 62, 100);
    int scans = 1;
    CHECK_FALSE (pending.forEachNote ({}, 2, scans, [] (const auto&) { return true; }));
    CHECK (scans == 0);
    pending.pushEvent (1, 3, 0x90, 64, 100);
    pending.publishProgress (1, 100, true);
    CHECK (pending.takeOverflow());
    CHECK_FALSE (pending.takeOverflow());
    CHECK (notesFor (pending, {}, 2).empty());
    pending.pushEvent (2, 4, 0x90, 67, 90);
    CHECK (notesFor (pending, {}, 3).empty());
    pending.configure (43, 100, {});
    pending.pushEvent (1, 4, 0x90, 67, 90);
    pending.pushEvent (1, 9, 0x80, 67, 0);
    CHECK (notesFor (pending, {}, 2) == std::vector<MidiNote> { { 1, 67, 90, 4, 5 } });
    pending.recoverGap();
    CHECK (pending.takeOverflow());
    CHECK (notesFor (pending, {}, 2).empty());
}

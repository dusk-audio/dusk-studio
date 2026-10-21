#include <catch2/catch_test_macros.hpp>

#include "engine/MidiCaptureStream.h"
#include "engine/MidiMergePlayback.h"

#include <algorithm>
#include <cmath>
#include <utility>
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

TEST_CASE ("Pending MIDI Merge kind indexes preserve payloads across consumption and wrapping",
           "[midi][recording][merge][issue-905]")
{
    SECTION ("mixed records repeatedly wrap and early visitor refusal does not consume them")
    {
        MidiMergePlayback pending (8);
        pending.configure (42, 100, {});
        std::vector<MidiNote> expectedNotes;
        std::vector<MidiCc> expectedControls;
        const std::uint8_t messages[][3] { { 0xb0, 64, 127 }, { 0xe1, 1, 96 }, { 0xd2, 91, 0 },
                                          { 0xa3, 60, 80 }, { 0xc4, 12, 0 } };
        for (int pass = 1; pass <= 100; ++pass)
        {
            pending.discardConsumed ({ 42, pass - 2 });
            if (pass > 2)
            {
                expectedNotes.erase (expectedNotes.begin());
                expectedControls.erase (expectedControls.begin(), expectedControls.begin() + 2);
            }
            pending.pushEvent (pass, 0, 0x90, (std::uint8_t) (pass & 127), 100);
            for (int event = 0; event < 2; ++event)
            {
                const auto& bytes = messages[(pass + event) % 5];
                const int at = (pass * 37 + event * 23) % 100;
                pending.pushEvent (pass, at, bytes[0], bytes[1], bytes[2]);
                MidiCc control;
                REQUIRE (MidiCc::fromMessage (bytes, (bytes[0] & 0xf0) == 0xc0 || (bytes[0] & 0xf0) == 0xd0 ? 2 : 3,
                                              at, control));
                expectedControls.push_back (control);
            }
            pending.pushEvent (pass, 30, 0x80, (std::uint8_t) (pass & 127), 0);
            pending.publishProgress (pass, 100, true);
            expectedNotes.push_back ({ 1, pass & 127, 100, 0, 30 });
            CHECK_FALSE (pending.takeOverflow());
            CHECK (notesFor (pending, {}, pass + 1) == expectedNotes);
            int scans = 1000;
            CHECK_FALSE (pending.forEachControl ({}, pass + 1, scans, [] (const auto&) { return false; }));
            std::vector<MidiCc> actual;
            REQUIRE (pending.forEachControl ({}, pass + 1, scans, [&] (const auto& control)
            { actual.push_back (control); return true; }));
            CHECK (actual == expectedControls);
            const auto wanted = expectedControls.front().atTick;
            std::vector<MidiCc> filtered, expectedFiltered;
            for (const auto& control : expectedControls)
                if (control.atTick == wanted) expectedFiltered.push_back (control);
            REQUIRE (pending.forEachControl ({}, pass + 1, scans, [&] (const MidiCc& control)
            {
                if (control.atTick == wanted) filtered.push_back (control);
                return true;
            }, [wanted] (std::int64_t first, std::int64_t last) { return first <= wanted && last >= wanted; }));
            CHECK (filtered == expectedFiltered);
        }
    }
    SECTION ("filtered chunks agree with complete payloads across partial consumption and metadata reuse")
    {
        MidiMergePlayback pending (200);
        pending.configure (42, 1000, {});
        for (int pass = 1; pass <= 16; ++pass)
        {
            pending.discardConsumed ({ 42, pass - 2 });
            const int base = pass % 2 == 0 ? 700 : 100;
            for (int event = 0; event < 96; ++event)
                pending.pushEvent (pass, base + (event * 17) % 96,
                                   (std::uint8_t) (0xd0 | (pass & 15)), (std::uint8_t) ((event * 3) & 127), 0);
            pending.publishProgress (pass, 1000, true);
            CHECK_FALSE (pending.takeOverflow());
            int scans = 10000;
            std::vector<MidiCc> complete;
            REQUIRE (pending.forEachControl ({}, pass + 1, scans, [&] (const MidiCc& control)
            { complete.push_back (control); return true; }));
            REQUIRE (complete.size() == (pass == 1 ? 96 : 192));
            for (const auto window : { std::pair<int, int> { 0, 50 }, { 100, 200 }, { 700, 800 }, { 150, 751 } })
            {
                std::vector<MidiCc> expected, actual;
                for (const auto& control : complete)
                    if (control.atTick >= window.first && control.atTick < window.second) expected.push_back (control);
                REQUIRE (pending.forEachControl ({}, pass + 1, scans, [&] (const MidiCc& control)
                {
                    if (control.atTick >= window.first && control.atTick < window.second) actual.push_back (control);
                    return true;
                }, [window] (std::int64_t first, std::int64_t last)
                { return last >= window.first && first < window.second; }));
                CHECK (actual == expected);
            }
        }
    }
    SECTION ("consumed and future controller history does not consume the note or control scan budget")
    {
        MidiMergePlayback pending (20000);
        pending.configure (42, 20000, {});
        for (int at = 0; at < 10000; ++at) pending.pushEvent (1, at, 0xd0, (std::uint8_t) (at & 127), 0);
        pending.pushEvent (2, 1, 0x90, 60, 100);
        pending.pushEvent (2, 2, 0x80, 60, 0);
        for (int at = 0; at < 9998; ++at) pending.pushEvent (3, at, 0xe0, 0, 64);
        CHECK (notesFor (pending, { 42, 1 }, 3, 128) == std::vector<MidiNote> { { 1, 60, 100, 1, 1 } });
        int scans = 128, controls = 0;
        REQUIRE (pending.forEachControl ({ 42, 1 }, 3, scans, [&] (const auto&) { ++controls; return true; }));
        CHECK (controls == 0);
    }
    SECTION ("combined capacity, gap recovery and reconfiguration reset both views")
    {
        MidiMergePlayback pending (4);
        pending.configure (42, 100, {});
        for (int key = 60; key < 62; ++key)
        {
            pending.pushEvent (1, 0, 0x90, (std::uint8_t) key, 100);
            pending.pushEvent (1, 10, 0xb0, 1, (std::uint8_t) key);
            pending.pushEvent (1, 20, 0x80, (std::uint8_t) key, 0);
        }
        REQUIRE (notesFor (pending, {}, 2).size() == 2);
        CHECK_FALSE (pending.takeOverflow());
        pending.pushEvent (1, 30, 0xb0, 1, 0);
        CHECK (pending.takeOverflow());
        CHECK_FALSE (pending.takeOverflow());
        CHECK (notesFor (pending, {}, 2).empty());
        int scans = 100, controls = 0;
        REQUIRE (pending.forEachControl ({}, 2, scans, [&] (const auto&) { ++controls; return true; }));
        CHECK (controls == 0);
        pending.configure (43, 100, {});
        pending.pushEvent (1, 10, 0xb0, 1, 42);
        REQUIRE (pending.forEachControl ({}, 2, scans, [&] (const auto&) { ++controls; return true; }));
        CHECK (controls == 1);
        pending.recoverGap();
        REQUIRE (pending.forEachControl ({}, 2, scans, [&] (const auto&) { ++controls; return true; }));
        CHECK (controls == 1);
    }
}

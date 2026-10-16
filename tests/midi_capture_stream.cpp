#include <catch2/catch_test_macros.hpp>

#include "engine/MidiCaptureStream.h"

#include <algorithm>
#include <cstdint>

using namespace duskstudio;

TEST_CASE ("MIDI capture preview extends held notes only through published progress",
           "[recording][midi][capture-stream]")
{
    SECTION ("Held lengths follow progress and stop growing after release")
    {
        MidiCaptureStream stream;
        stream.configure ([] (std::int64_t sample) { return sample / 2; });
        const auto initialGeneration = stream.preview().generation;
        REQUIRE (stream.pushEvent (1, 10, 0x92, 64, 87));
        REQUIRE (stream.publishProgress (1, 30, false));
        REQUIRE (stream.service());
        const auto first = stream.preview();
        REQUIRE (first.generation > initialGeneration);
        REQUIRE (first.passOrdinal == 1);
        REQUIRE (first.lengthInSamples == 30);
        REQUIRE (first.lengthInTicks == 15);
        REQUIRE (first.notes == std::vector<MidiNote> { { 3, 64, 87, 5, 10 } });
        REQUIRE (stream.takeCompleted().empty());
        REQUIRE_FALSE (stream.service());
        REQUIRE (stream.preview().generation == first.generation);

        REQUIRE (stream.publishProgress (1, 50, false));
        stream.service();
        REQUIRE (stream.preview().notes == std::vector<MidiNote> { { 3, 64, 87, 5, 20 } });
        REQUIRE (stream.pushEvent (1, 60, 0x82, 64, 0));
        REQUIRE (stream.publishProgress (1, 80, false));
        stream.service();
        REQUIRE (stream.preview().notes == std::vector<MidiNote> { { 3, 64, 87, 5, 25 } });
    }
    SECTION ("Progress reuses closed-note and controller history")
    {
        MidiCaptureStream stream;
        constexpr int history = 512;
        for (int i = 0; i < history; ++i)
        {
            REQUIRE (stream.pushEvent (1, i * 4, 0x90, 60, 100));
            REQUIRE (stream.pushEvent (1, i * 4 + 1, 0x80, 60, 0));
            REQUIRE (stream.pushEvent (1, i * 4 + 2, 0xb0, 1, (std::uint8_t) (i & 127)));
        }
        REQUIRE (stream.pushEvent (1, history * 4, 0x90, 67, 90));
        REQUIRE (stream.publishProgress (1, history * 4 + 10, false));
        REQUIRE (stream.service());
        const auto first = stream.preview();
        REQUIRE (first.notes.size() == history + 1);
        REQUIRE (first.ccs.size() == history);
        const auto* noteStorage = stream.preview().notes.data();
        const auto* controlStorage = stream.preview().ccs.data();
        const auto noteCapacity = stream.preview().notes.capacity();
        const auto controlCapacity = stream.preview().ccs.capacity();
        for (int progress = 11; progress <= 30; ++progress)
        {
            REQUIRE (stream.publishProgress (1, history * 4 + progress, false));
            REQUIRE (stream.service());
            const auto& preview = stream.preview();
            REQUIRE (preview.notes.data() == noteStorage);
            REQUIRE (preview.ccs.data() == controlStorage);
            REQUIRE (preview.notes.capacity() == noteCapacity);
            REQUIRE (preview.ccs.capacity() == controlCapacity);
            REQUIRE (preview.notes.size() == first.notes.size());
            REQUIRE (std::equal (first.notes.begin(), first.notes.end() - 1, preview.notes.begin()));
            REQUIRE (preview.ccs == first.ccs);
            REQUIRE (preview.notes.back() == MidiNote { 1, 67, 90, history * 4, progress });
            REQUIRE (preview.lengthInSamples == history * 4 + progress);
            REQUIRE (preview.lengthInTicks == history * 4 + progress);
            REQUIRE (preview.generation > first.generation);
            REQUIRE (std::is_sorted (preview.notes.begin(), preview.notes.end(),
                                     [] (const auto& a, const auto& b) { return a.startTick < b.startTick; }));
        }
    }
    SECTION ("Only the held note grows when retriggers share its rounded tick")
    {
        MidiCaptureStream stream;
        stream.configure ([] (std::int64_t sample) { return sample / 10; });
        REQUIRE (stream.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE (stream.pushEvent (1, 1, 0x80, 60, 0));
        REQUIRE (stream.pushEvent (1, 2, 0x90, 60, 100));
        REQUIRE (stream.pushEvent (1, 2, 0x90, 60, 100));
        REQUIRE (stream.publishProgress (1, 5, false));
        REQUIRE (stream.service());
        REQUIRE (stream.preview().notes == std::vector<MidiNote> (3, { 1, 60, 100, 0, 1 }));
        REQUIRE (stream.publishProgress (1, 20, false));
        REQUIRE (stream.service());
        REQUIRE (stream.preview().notes == std::vector<MidiNote> {
            { 1, 60, 100, 0, 1 }, { 1, 60, 100, 0, 1 }, { 1, 60, 100, 0, 2 } });
    }
    SECTION ("Progress reveals a held onset at the previous bound")
    {
        MidiCaptureStream stream;
        REQUIRE (stream.pushEvent (1, 10, 0x90, 60, 100));
        REQUIRE (stream.publishProgress (1, 10, false));
        REQUIRE (stream.service());
        REQUIRE (stream.preview().notes.empty());
        REQUIRE (stream.publishProgress (1, 11, false));
        REQUIRE (stream.service());
        REQUIRE (stream.preview().notes == std::vector<MidiNote> { { 1, 60, 100, 10, 1 } });
    }
}

TEST_CASE ("MIDI capture service drains its initial snapshot before later completion",
           "[recording][midi][capture-stream]")
{
    MidiCaptureStream stream (16);
    bool injectLaterSpan = false;
    stream.configure ([&] (std::int64_t sample)
    {
        if (injectLaterSpan)
        {
            injectLaterSpan = false;
            REQUIRE (stream.pushEvent (1, 15, 0x80, 60, 0));
            REQUIRE (stream.publishProgress (1, 20, true));
        }
        return sample;
    }, 20);
    stream.pushEvent (1, 0, 0x90, 60, 100);
    stream.publishProgress (1, 10, false);
    injectLaterSpan = true;
    REQUIRE (stream.service());
    REQUIRE (stream.takeCompleted().empty());
    REQUIRE (stream.preview().passOrdinal == 1);
    REQUIRE (stream.preview().lengthInSamples == 10);
    REQUIRE (stream.preview().notes == std::vector<MidiNote> { { 1, 60, 100, 0, 10 } });

    REQUIRE (stream.service());
    const auto passes = stream.takeCompleted();
    REQUIRE (passes.size() == 1);
    REQUIRE (passes[0].notes == std::vector<MidiNote> { { 1, 60, 100, 0, 15 } });
    REQUIRE (passes[0].lengthInSamples == 20);
    REQUIRE_FALSE (stream.service());
}

TEST_CASE ("MIDI capture publishes empty spans and empty loop passes without controls",
           "[recording][midi][capture-stream]")
{
    MidiCaptureStream stream;
    stream.configure ({}, 100);
    stream.publishProgress (1, 40, false);
    stream.service();
    REQUIRE (stream.preview().lengthInSamples == 40);
    REQUIRE (stream.preview().notes.empty());
    REQUIRE (stream.preview().ccs.empty());
    stream.pushEvent (1, 50, 0xb0, 64, 127);
    stream.publishProgress (1, 100, true);
    stream.publishProgress (2, 100, true);
    stream.publishProgress (3, 25, false);
    stream.service();

    const auto passes = stream.takeCompleted();
    REQUIRE (passes.size() == 2);
    REQUIRE (passes[0].passOrdinal == 1);
    REQUIRE (passes[0].ccs == std::vector<MidiCc> { { 1, 64, 127, 50 } });
    REQUIRE (passes[1].passOrdinal == 2);
    REQUIRE (passes[1].lengthInSamples == 100);
    REQUIRE (passes[1].notes.empty());
    REQUIRE (passes[1].ccs.empty());
    REQUIRE (stream.preview().passOrdinal == 3);
    REQUIRE (stream.preview().lengthInSamples == 25);
    REQUIRE (stream.preview().ccs.empty());
}

TEST_CASE ("MIDI capture delayed drains keep several loop performances separate",
           "[recording][midi][capture-stream]")
{
    MidiCaptureStream stream;
    stream.configure ({}, 100);
    for (int pass = 1; pass <= 4; ++pass)
    {
        stream.pushEvent (pass, pass * 5, 0x90, (std::uint8_t) (60 + pass), 100);
        stream.pushEvent (pass, pass * 5 + 20, 0x80, (std::uint8_t) (60 + pass), 0);
        stream.publishProgress (pass, 100, true);
    }
    stream.publishProgress (5, 10, false);
    stream.service();
    const auto passes = stream.takeCompleted();
    REQUIRE (passes.size() == 4);
    for (int pass = 1; pass <= 4; ++pass)
    {
        const auto& result = passes[(std::size_t) (pass - 1)];
        REQUIRE (result.passOrdinal == pass);
        REQUIRE (result.notes == std::vector<MidiNote> { { 1, 60 + pass, 100, pass * 5, 20 } });
        REQUIRE (result.lengthInSamples == 100);
    }
    REQUIRE (stream.preview().passOrdinal == 5);
    REQUIRE (stream.preview().notes.empty());
    REQUIRE (stream.takeCompleted().empty());
}

TEST_CASE ("MIDI capture held notes retrigger across loop seams until their release",
           "[recording][midi][capture-stream]")
{
    MidiCaptureStream stream;
    stream.configure ({}, 100);
    stream.pushEvent (1, 70, 0x9f, 69, 91);
    stream.publishProgress (1, 100, true);
    stream.publishProgress (2, 100, true);
    stream.pushEvent (3, 25, 0x9f, 69, 0);
    stream.publishProgress (3, 50, false);
    stream.service();
    const auto passes = stream.takeCompleted();
    REQUIRE (passes.size() == 2);
    REQUIRE (passes[0].notes == std::vector<MidiNote> { { 16, 69, 91, 70, 30 } });
    REQUIRE (passes[1].notes == std::vector<MidiNote> { { 16, 69, 91, 0, 100 } });
    REQUIRE (stream.preview().notes == std::vector<MidiNote> { { 16, 69, 91, 0, 25 } });
    stream.sealFinal (3, 50);
    const auto final = stream.takeCompleted();
    REQUIRE (final.size() == 1);
    REQUIRE (final[0].notes == std::vector<MidiNote> { { 16, 69, 91, 0, 25 } });
}

TEST_CASE ("MIDI capture repeated onsets close the previous note before velocity-zero release",
           "[recording][midi][capture-stream]")
{
    MidiCaptureStream stream;
    stream.pushEvent (1, 10, 0x90, 60, 80);
    stream.pushEvent (1, 20, 0x90, 60, 100);
    stream.pushEvent (1, 35, 0x90, 60, 0);
    stream.pushEvent (1, 36, 0x90, 62, 90);
    stream.pushEvent (1, 36, 0x90, 62, 70);
    stream.pushEvent (1, 36, 0x90, 62, 0);
    stream.publishProgress (1, 40, false);
    stream.service();
    REQUIRE (stream.preview().notes == std::vector<MidiNote> {
        { 1, 60, 80, 10, 10 }, { 1, 60, 100, 20, 15 },
        { 1, 62, 90, 36, 1 }, { 1, 62, 70, 36, 1 } });
    stream.sealFinal (1, 50);
    REQUIRE (stream.takeCompleted()[0].notes.size() == 4);
}

TEST_CASE ("MIDI capture decodes every tagged channel control and filters malformed events",
           "[recording][midi][capture-stream]")
{
    MidiCaptureStream stream;
    stream.configure ({}, 100);
    stream.pushEvent (1, 10, 0xb1, 7, 100);
    stream.pushEvent (1, 11, 0xe2, 1, 64);
    stream.pushEvent (1, 12, 0xd3, 70, 0);
    stream.pushEvent (1, 13, 0xa4, 60, 80);
    stream.pushEvent (1, 14, 0xc5, 9, 0);
    stream.pushEvent (1, 15, 0xf1, 1, 0);
    stream.pushEvent (1, 16, 0x01, 1, 0);
    stream.pushEvent (1, 17, 0x90, 128, 90);
    stream.pushEvent (1, 18, 0x90, 60, 128);
    stream.pushEvent (1, 19, 0xe0, 1, 128);
    stream.pushEvent (1, 20, 0xc0, 128, 0);
    stream.pushEvent (1, -1, 0xb0, 7, 100);
    stream.pushEvent (1, 100, 0xb0, 7, 100);
    stream.pushEvent (0, 21, 0xb0, 7, 100);
    stream.publishProgress (1, 100, true);
    stream.service();
    const auto passes = stream.takeCompleted();
    REQUIRE (passes.size() == 1);
    REQUIRE (passes[0].notes.empty());
    REQUIRE (passes[0].ccs == std::vector<MidiCc> {
        { 2, 7, 100, 10, MidiCc::Kind::ControlChange },
        { 3, 0, 8193, 11, MidiCc::Kind::PitchBend },
        { 4, 0, 70, 12, MidiCc::Kind::ChannelPressure },
        { 5, 60, 80, 13, MidiCc::Kind::PolyPressure },
        { 6, 0, 9, 14, MidiCc::Kind::ProgramChange } });
}

TEST_CASE ("MIDI capture overflow retains queued data and truncates a dropped note release",
           "[recording][midi][capture-stream]")
{
    SECTION ("Dropped progress preserves a held note until Stop")
    {
        MidiCaptureStream stream (1);
        REQUIRE (stream.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE_FALSE (stream.publishProgress (1, 10, false));
        stream.service();
        stream.sealFinal (1, 20);
        REQUIRE (stream.overflowCount() == 0);
        const auto passes = stream.takeCompleted();
        REQUIRE (passes.size() == 1);
        REQUIRE (passes[0].notes == std::vector<MidiNote> { { 1, 60, 100, 0, 20 } });
    }
    SECTION ("Dropped completion preserves the held note and its next-pass carry")
    {
        MidiCaptureStream stream (1);
        stream.configure ({}, 100);
        REQUIRE (stream.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE_FALSE (stream.publishProgress (1, 100, true));
        stream.service();
        const auto passes = stream.takeCompleted();
        REQUIRE (passes.size() == 1);
        REQUIRE (passes[0].notes == std::vector<MidiNote> { { 1, 60, 100, 0, 100 } });
        REQUIRE (stream.publishProgress (2, 25, false));
        stream.service();
        REQUIRE (stream.preview().notes == std::vector<MidiNote> { { 1, 60, 100, 0, 25 } });
        REQUIRE (stream.pushEvent (2, 30, 0x80, 60, 0));
        REQUIRE_FALSE (stream.publishProgress (2, 40, false));
        stream.sealFinal (2, 50);
        const auto final = stream.takeCompleted();
        REQUIRE (final.size() == 1);
        REQUIRE (final[0].notes == std::vector<MidiNote> { { 1, 60, 100, 0, 30 } });
        REQUIRE (stream.overflowCount() == 0);
    }
    SECTION ("A dropped note release truncates the held note")
    {
        MidiCaptureStream stream (3);
        stream.configure ({}, 100);
        REQUIRE (stream.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE (stream.publishProgress (1, 10, false));
        REQUIRE (stream.pushEvent (1, 8, 0xb0, 7, 100));
        REQUIRE_FALSE (stream.pushEvent (1, 20, 0x80, 60, 0));
        REQUIRE_FALSE (stream.publishProgress (1, 100, true));
        REQUIRE (stream.overflowCount() == 1);
        stream.service();
        const auto passes = stream.takeCompleted();
        REQUIRE (passes.size() == 1);
        REQUIRE (passes[0].lengthInSamples == 100);
        REQUIRE (passes[0].notes == std::vector<MidiNote> { { 1, 60, 100, 0, 10 } });
        REQUIRE (passes[0].ccs == std::vector<MidiCc> { { 1, 7, 100, 8 } });
        REQUIRE (stream.preview().passOrdinal == 2);
        REQUIRE (stream.preview().notes.empty());
        stream.pushEvent (2, 30, 0x90, 64, 80);
        stream.publishProgress (2, 40, false);
        stream.service();
        REQUIRE (stream.preview().notes == std::vector<MidiNote> { { 1, 64, 80, 30, 10 } });
    }
}

TEST_CASE ("MIDI capture gaps cannot pair later note-offs with stale held notes",
           "[recording][midi][capture-stream]")
{
    MidiCaptureStream stream (2);
    stream.pushEvent (1, 0, 0x90, 60, 100);
    stream.publishProgress (1, 10, false);
    REQUIRE_FALSE (stream.pushEvent (1, 15, 0x80, 60, 0));
    stream.service();
    stream.pushEvent (1, 20, 0x80, 60, 0);
    stream.publishProgress (1, 30, false);
    stream.service();
    REQUIRE (stream.preview().notes == std::vector<MidiNote> { { 1, 60, 100, 0, 10 } });
    stream.pushEvent (1, 40, 0x90, 60, 70);
    stream.publishProgress (1, 50, false);
    stream.service();
    REQUIRE (stream.preview().notes == std::vector<MidiNote> {
        { 1, 60, 100, 0, 10 }, { 1, 60, 70, 40, 10 } });
}

TEST_CASE ("MIDI capture recovers several dropped empty loop completions exactly once",
           "[recording][midi][capture-stream]")
{
    MidiCaptureStream stream (1);
    stream.configure ({}, 100);
    REQUIRE (stream.publishProgress (1, 100, true));
    REQUIRE_FALSE (stream.publishProgress (2, 100, true));
    REQUIRE_FALSE (stream.publishProgress (3, 100, true));
    REQUIRE (stream.overflowCount() == 0);
    stream.service();
    const auto passes = stream.takeCompleted();
    REQUIRE (passes.size() == 3);
    for (int index = 0; index < 3; ++index)
    {
        REQUIRE (passes[(std::size_t) index].passOrdinal == index + 1);
        REQUIRE (passes[(std::size_t) index].lengthInSamples == 100);
        REQUIRE (passes[(std::size_t) index].notes.empty());
        REQUIRE (passes[(std::size_t) index].ccs.empty());
    }
    REQUIRE_FALSE (stream.service());
    stream.sealFinal (3, 100);
    stream.sealFinal (3, 100);
    REQUIRE (stream.takeCompleted().empty());
}

TEST_CASE ("MIDI capture stop seals a partial pass and clips events beyond its final bound",
           "[recording][midi][capture-stream]")
{
    MidiCaptureStream stream;
    stream.pushEvent (1, 10, 0x90, 60, 100);
    stream.pushEvent (1, 20, 0xb0, 1, 50);
    stream.publishProgress (1, 40, false);
    stream.pushEvent (1, 70, 0x80, 60, 0);
    stream.pushEvent (1, 75, 0x90, 62, 80);
    stream.pushEvent (1, 80, 0xb0, 1, 70);
    stream.sealFinal (1, 50);
    const auto passes = stream.takeCompleted();
    REQUIRE (passes.size() == 1);
    REQUIRE (passes[0].lengthInSamples == 50);
    REQUIRE (passes[0].notes == std::vector<MidiNote> { { 1, 60, 100, 10, 40 } });
    REQUIRE (passes[0].ccs == std::vector<MidiCc> { { 1, 1, 50, 20 } });
    stream.sealFinal (1, 50);
    REQUIRE_FALSE (stream.service());
    REQUIRE (stream.takeCompleted().empty());
    REQUIRE (stream.preview().notes.empty());
}

TEST_CASE ("MIDI capture configure resets an earlier gesture including its overflow",
           "[recording][midi][capture-stream]")
{
    MidiCaptureStream stream (1);
    stream.pushEvent (1, 0, 0x90, 60, 100);
    REQUIRE_FALSE (stream.publishProgress (1, 10, false));
    stream.sealFinal (1, 20);
    stream.configure ([] (std::int64_t sample) { return sample * 3; });
    REQUIRE (stream.overflowCount() == 0);
    REQUIRE (stream.takeCompleted().empty());
    REQUIRE (stream.preview().notes.empty());
    stream.pushEvent (1, 10, 0xb0, 1, 70);
    stream.service();
    stream.publishProgress (1, 30, false);
    stream.sealFinal (1, 40);
    const auto passes = stream.takeCompleted();
    REQUIRE (passes.size() == 1);
    REQUIRE (passes[0].lengthInTicks == 120);
    REQUIRE (passes[0].ccs == std::vector<MidiCc> { { 1, 1, 70, 30 } });
}

TEST_CASE ("MIDI capture retains events before a sample boundary that rounds to the final tick",
           "[recording][midi][capture-stream]")
{
    MidiCaptureStream stream;
    stream.configure ([] (std::int64_t sample) { return (sample + 5) / 10; });
    stream.pushEvent (1, 48, 0x90, 60, 100);
    stream.pushEvent (1, 49, 0x80, 60, 0);
    stream.pushEvent (1, 49, 0xe0, 1, 64);
    stream.pushEvent (1, 50, 0xc0, 7, 0);
    stream.pushEvent (1, 51, 0x90, 62, 80);
    stream.pushEvent (1, 52, 0x80, 62, 0);
    stream.sealFinal (1, 50);
    const auto passes = stream.takeCompleted();
    REQUIRE (passes.size() == 1);
    REQUIRE (passes[0].lengthInTicks == 5);
    REQUIRE (passes[0].notes == std::vector<MidiNote> { { 1, 60, 100, 4, 1 } });
    REQUIRE (passes[0].ccs == std::vector<MidiCc> {
        { 1, 0, 8193, 4, MidiCc::Kind::PitchBend } });

    stream.configure ([] (std::int64_t) { return 0; });
    stream.pushEvent (1, 0, 0x90, 64, 90);
    stream.pushEvent (1, 0, 0xb0, 64, 127);
    stream.sealFinal (1, 1);
    const auto shortPass = stream.takeCompleted();
    REQUIRE (shortPass[0].lengthInTicks == 1);
    REQUIRE (shortPass[0].notes == std::vector<MidiNote> { { 1, 64, 90, 0, 1 } });
    REQUIRE (shortPass[0].ccs == std::vector<MidiCc> { { 1, 64, 127, 0 } });
}

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_basics/juce_audio_basics.h>

#include "TestTempDirectory.h"
#include "engine/MidiTimelineSchedule.h"
#include "engine/midi/MidiFileReader.h"
#include "foundation/Fs.h"
#include "session/MidiRecovery.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace duskstudio;
namespace recovery = duskstudio::midirecovery;
namespace stdfs = std::filesystem;

namespace
{
constexpr double kRate = 48000.0;
using Bytes = std::vector<std::uint8_t>;

// One event as it sits in a file, or as playback sends it with `at` in samples.
struct Wire
{
    std::int64_t at;
    int status;
    int data1;
    int data2;

    bool operator== (const Wire& o) const noexcept
    {
        return at == o.at && status == o.status && data1 == o.data1 && data2 == o.data2;
    }
    bool operator< (const Wire& o) const noexcept
    {
        return std::tie (at, status, data1, data2) < std::tie (o.at, o.status, o.data1, o.data2);
    }
};

std::ostream& operator<< (std::ostream& os, const Wire& w)
{
    return os << "{ " << w.at << ", 0x" << std::hex << w.status << std::dec
              << ", " << w.data1 << ", " << w.data2 << " }";
}

MidiNote note (int channel, int key, int velocity, std::int64_t start, std::int64_t length)
{
    return { channel, key, velocity, start, length };
}

MidiCc control (MidiCc::Kind kind, int channel, int controller, int value, std::int64_t at)
{
    return { channel, controller, value, at, kind };
}

std::chrono::system_clock::time_point utc (std::int64_t secondsSinceEpoch)
{
    return std::chrono::system_clock::time_point (std::chrono::seconds (secondsSinceEpoch));
}

constexpr std::int64_t kNovember8th143005 = 1794148205;   // 2026-11-08 14:30:05 UTC

std::vector<std::vector<midi::MidiFileEvent>> tracksOf (const Bytes& file)
{
    midi::MidiFileReader reader;
    REQUIRE (reader.readData (file.data(), file.size()));
    REQUIRE (reader.timeFormat() == kMidiTicksPerQuarter);
    return reader.tracks();
}

std::vector<Wire> channelEvents (const std::vector<midi::MidiFileEvent>& events)
{
    std::vector<Wire> out;
    for (const auto& ev : events)
        if (ev.isChannelMessage())
            out.push_back ({ ev.tick, ev.status, ev.data1, ev.data2 });
    return out;
}

std::int64_t lastTickOf (const Bytes& file)
{
    std::int64_t last = 0;
    for (const auto& track : tracksOf (file))
        for (const auto& ev : track)
            last = std::max (last, ev.tick);
    return last;
}

// The JUCE-free reader drops meta payloads; JUCE's reader keeps them.
juce::MidiFile juceRead (const Bytes& file)
{
    juce::MidiFile midiFile;
    juce::MemoryInputStream in (file.data(), file.size(), false);
    REQUIRE (midiFile.readFrom (in));
    return midiFile;
}

std::vector<std::string> trackNames (const Bytes& file)
{
    const auto midiFile = juceRead (file);
    std::vector<std::string> names;
    for (int t = 0; t < midiFile.getNumTracks(); ++t)
    {
        const auto& sequence = *midiFile.getTrack (t);
        std::string name;
        for (int i = 0; i < sequence.getNumEvents() && name.empty(); ++i)
            if (sequence.getEventPointer (i)->message.isTrackNameEvent())
                name = sequence.getEventPointer (i)->message.getTextFromTextMetaEvent().toStdString();
        names.push_back (name);
    }
    return names;
}

// Tempo changes as { tick, microseconds a quarter, 0, 0 }.
std::vector<Wire> temposOf (const Bytes& file)
{
    juce::MidiMessageSequence tempos;
    juceRead (file).findAllTempoEvents (tempos);
    std::vector<Wire> out;
    for (int i = 0; i < tempos.getNumEvents(); ++i)
    {
        const auto& message = tempos.getEventPointer (i)->message;
        out.push_back ({ (std::int64_t) std::llround (message.getTimeStamp()),
                         (int) std::llround (message.getTempoSecondsPerQuarterNote() * 1.0e6), 0, 0 });
    }
    return out;
}

std::pair<int, int> meterOf (const Bytes& file)
{
    juce::MidiMessageSequence meters;
    juceRead (file).findAllTimeSigEvents (meters);
    REQUIRE (meters.getNumEvents() == 1);
    int numerator = 0;
    int denominator = 0;
    meters.getEventPointer (0)->message.getTimeSignatureInfo (numerator, denominator);
    return { numerator, denominator };
}

// What playback sends for `regions` over [0, end), each event at its timeline
// sample. A program change or channel pressure carries no second data byte.
std::vector<Wire> playbackOf (const std::vector<MidiRegion>& regions, const TempoMap* map, float bpm,
                              std::int64_t end)
{
    const auto timeline = buildMidiTimeline (regions);
    const auto chase = std::make_unique<midischedule::ControllerChase>();
    std::vector<Wire> out;
    int scans = 1 << 20;
    const auto outcome = midischedule::scheduleSpan (
        *timeline, map, kRate, bpm, { 0, end, false }, scans, *chase,
        [&out] (std::uint8_t status, std::uint8_t data1, std::uint8_t data2, std::int64_t at)
        {
            const int kind = status & 0xf0;
            out.push_back ({ at, status, data1, kind == 0xc0 || kind == 0xd0 ? 0 : data2 });
            return true;
        });
    REQUIRE (outcome == midischedule::Outcome::complete);
    std::sort (out.begin(), out.end());
    return out;
}

// A file's channel events with `at` turned from ticks to timeline samples.
std::vector<Wire> inSamples (std::vector<Wire> events, const TempoMap* map, float bpm)
{
    for (auto& ev : events)
        ev.at = map != nullptr ? map->ticksToSamples (ev.at, kRate) : ticksToSamples (ev.at, kRate, bpm);
    std::sort (events.begin(), events.end());
    return events;
}

Bytes contentOf (const stdfs::path& file)
{
    const auto text = dusk::fs::loadFileAsString (file);
    return Bytes (text.begin(), text.end());
}

std::vector<std::string> namesIn (const stdfs::path& folder)
{
    std::vector<std::string> names;
    for (const auto& entry : stdfs::directory_iterator (folder))
        names.push_back (entry.path().filename().u8string());
    std::sort (names.begin(), names.end());
    return names;
}

// 120 bpm, then 90 from two seconds in, where the map reaches tick 1920.
recovery::Timing twoTempoTiming()
{
    recovery::Timing timing;
    timing.sampleRate = kRate;
    timing.tempoMap.setPoints ({ { 0, 120.0f }, { 96000, 90.0f } });
    return timing;
}

// A track's regions, stored out of timeline order: a muted intro, a verse with
// two previous takes, a muted chorus with one, and an outro.
std::vector<MidiRegion> fourRegions()
{
    const auto region = [] (std::int64_t timelineStart, bool muted, const char* label)
    {
        MidiRegion made;
        made.timelineStart = timelineStart;
        made.lengthInTicks = 1200;
        made.lengthInSamples = ticksToSamples (1200, kRate, 120.0f);
        made.muted = muted;
        made.label = label;
        return made;
    };

    auto intro = region (24000, true, "");   // tick 480
    intro.notes.push_back (note (10, 36, 127, 0, 120));

    auto verse = region (96000, false, "Verse");   // tick 1920
    verse.notes.push_back (note (1, 60, 100, 0, 480));
    verse.previousTakes.resize (2);
    verse.previousTakes[0].notes.push_back (note (1, 62, 90, 120, 240));
    verse.previousTakes[1].ccs.push_back (control (MidiCc::Kind::ControlChange, 1, 1, 64, 30));

    auto chorus = region (192000, true, "Chorus");   // tick 3840
    chorus.notes.push_back (note (1, 67, 80, 0, 240));
    chorus.previousTakes.resize (1);
    chorus.previousTakes[0].notes.push_back (note (1, 69, 70, 60, 120));

    auto outro = region (288000, false, "");   // tick 5760
    outro.notes.push_back (note (2, 48, 110, 0, 960));
    outro.ccs.push_back (control (MidiCc::Kind::ControlChange, 2, 7, 90, 480));

    return { outro, verse, intro, chorus };
}
} // namespace

TEST_CASE ("A MIDI take file puts every event at the tick playback plays it on the tempo map",
           "[midi][recovery]")
{
    const auto timing = twoTempoTiming();

    // From 1.5 s in, tick 1440, across the tempo change at tick 1920.
    MidiRegion pass;
    pass.timelineStart = 72000;
    pass.lengthInTicks = 4800;
    pass.notes.push_back (note (1, 60, 100, 0, 240));
    pass.notes.push_back (note (2, 64, 90, 240, 720));
    pass.notes.push_back (note (1, 67, 80, 960, 480));
    pass.ccs.push_back (control (MidiCc::Kind::ControlChange, 1, 7, 100, 0));
    pass.ccs.push_back (control (MidiCc::Kind::ProgramChange, 2, 0, 5, 0));
    pass.ccs.push_back (control (MidiCc::Kind::PolyPressure, 1, 60, 50, 100));
    pass.ccs.push_back (control (MidiCc::Kind::PitchBend, 1, 0, 8292, 120));
    pass.ccs.push_back (control (MidiCc::Kind::ChannelPressure, 2, 0, 70, 360));
    pass.ccs.push_back (control (MidiCc::Kind::ControlChange, 2, 64, 127, 1200));

    const auto file = recovery::buildTakeFile (timing, pass, "Keys");
    const auto tracks = tracksOf (file);
    REQUIRE (tracks.size() == 2);
    const auto events = channelEvents (tracks[1]);
    CHECK (events == std::vector<Wire> {
        { 1440, 0xb0, 7, 100 },
        { 1440, 0xc1, 5, 0 },
        { 1440, 0x90, 60, 100 },
        { 1540, 0xa0, 60, 50 },
        { 1560, 0xe0, 100, 64 },
        { 1680, 0x80, 60, 0 },
        { 1680, 0x91, 64, 90 },
        { 1800, 0xd1, 70, 0 },
        { 2400, 0x81, 64, 0 },
        { 2400, 0x90, 67, 80 },
        { 2640, 0xb1, 64, 127 },
        { 2880, 0x80, 67, 0 },
    });
    CHECK (channelEvents (tracks[0]).empty());

    const std::vector<MidiRegion> regions (1, pass);
    CHECK (inSamples (events, &timing.tempoMap, 0.0f) == playbackOf (regions, &timing.tempoMap, 120.0f, 1000000));
}

TEST_CASE ("A MIDI take file's conductor carries the tempo map only up to its last event",
           "[midi][recovery]")
{
    const auto timing = twoTempoTiming();
    MidiRegion pass;
    pass.timelineStart = 24000;   // tick 480
    std::vector<Wire> expected { { 0, 500000, 0, 0 } };

    SECTION ("a take that ends before the change")
    {
        pass.notes.push_back (note (1, 60, 100, 0, 480));
    }

    SECTION ("a take that ends on the change")
    {
        pass.notes.push_back (note (1, 60, 100, 0, 1440));
        expected.push_back ({ 1920, 666667, 0, 0 });
    }

    SECTION ("a take that runs past the change")
    {
        pass.ccs.push_back (control (MidiCc::Kind::ControlChange, 1, 1, 10, 2400));
        expected.push_back ({ 1920, 666667, 0, 0 });
    }

    const auto file = recovery::buildTakeFile (timing, pass, "Keys");
    CHECK (temposOf (file) == expected);

    const auto tracks = tracksOf (file);
    REQUIRE (tracks.size() == 2);
    const auto events = channelEvents (tracks[1]);
    REQUIRE_FALSE (events.empty());
    CHECK (lastTickOf (file) == events.back().at);
}

TEST_CASE ("A MIDI recovery file carries the session meter, a beat unit no power of two as quarters",
           "[midi][recovery]")
{
    auto session = std::make_unique<Session>();
    session->beatsPerBar.store (7);
    std::pair<int, int> expected;

    SECTION ("7/8")
    {
        session->beatUnit.store (8);
        expected = { 7, 8 };
    }

    SECTION ("7/6")
    {
        session->beatUnit.store (6);
        expected = { 7, 4 };
    }

    MidiRegion pass;
    pass.notes.push_back (note (1, 60, 100, 0, 480));
    CHECK (meterOf (recovery::buildTakeFile (recovery::timingOf (*session, kRate), pass, "Keys")) == expected);
}

TEST_CASE ("A MIDI playing file holds only the regions that play, numbered across all of the track's regions",
           "[midi][recovery]")
{
    const recovery::Timing timing;   // 120 bpm: 50 samples a tick
    const auto regions = fourRegions();

    const auto file = recovery::buildPlayingFile (timing, regions, "Keys");
    CHECK (trackNames (file) == std::vector<std::string> { "Keys", "Region 2 - Verse", "Region 4" });

    const auto tracks = tracksOf (file);
    REQUIRE (tracks.size() == 3);
    CHECK (channelEvents (tracks[1]) == std::vector<Wire> { { 1920, 0x90, 60, 100 }, { 2400, 0x80, 60, 0 } });
    CHECK (channelEvents (tracks[2]) == std::vector<Wire> {
        { 5760, 0x91, 48, 110 }, { 6240, 0xb1, 7, 90 }, { 6720, 0x81, 48, 0 },
    });

    // Merged into one region, as the importer merges a file's tracks, it is what
    // the track plays.
    std::vector<Wire> merged;
    for (const auto& track : tracks)
        for (const auto& event : channelEvents (track))
            merged.push_back (event);
    CHECK (inSamples (merged, nullptr, timing.bpm) == playbackOf (regions, nullptr, timing.bpm, 1000000));
}

TEST_CASE ("A MIDI takes file holds the muted regions and every previous take, numbered as the take menu numbers them",
           "[midi][recovery]")
{
    const recovery::Timing timing;

    const auto file = recovery::buildTakesFile (timing, fourRegions(), "Keys");
    CHECK (trackNames (file) == std::vector<std::string> {
        "Keys",
        "Region 1 (muted)",
        "Region 2 - Verse, take 2",
        "Region 2 - Verse, take 3",
        "Region 3 - Chorus, take 1 (muted)",
        "Region 3 - Chorus, take 2 (muted)",
    });

    const auto tracks = tracksOf (file);
    REQUIRE (tracks.size() == 6);
    CHECK (channelEvents (tracks[1]) == std::vector<Wire> { { 480, 0x99, 36, 127 }, { 600, 0x89, 36, 0 } });
    CHECK (channelEvents (tracks[2]) == std::vector<Wire> { { 2040, 0x90, 62, 90 }, { 2280, 0x80, 62, 0 } });
    CHECK (channelEvents (tracks[3]) == std::vector<Wire> { { 1950, 0xb0, 1, 64 } });
    CHECK (channelEvents (tracks[4]) == std::vector<Wire> { { 3840, 0x90, 67, 80 }, { 4080, 0x80, 67, 0 } });
    CHECK (channelEvents (tracks[5]) == std::vector<Wire> { { 3900, 0x90, 69, 70 }, { 4020, 0x80, 69, 0 } });
}

TEST_CASE ("A MIDI recovery file ends a note of no length one tick after it starts", "[midi][recovery]")
{
    const recovery::Timing timing;
    MidiRegion pass;
    pass.timelineStart = 48000;   // tick 960
    pass.lengthInTicks = 960;
    pass.lengthInSamples = ticksToSamples (960, kRate, timing.bpm);
    pass.notes.push_back (note (1, 60, 100, 240, 0));
    pass.notes.push_back (note (1, 62, 100, 480, -5));

    // Playback sends such a note's off on the sample of its on.
    const std::vector<MidiRegion> regions (1, pass);
    const auto played = playbackOf (regions, nullptr, timing.bpm, 1000000);
    CHECK (played == std::vector<Wire> {
        { 60000, 0x80, 60, 0 }, { 60000, 0x90, 60, 100 },
        { 72000, 0x80, 62, 0 }, { 72000, 0x90, 62, 100 },
    });

    const auto tracks = tracksOf (recovery::buildTakeFile (timing, pass, "Keys"));
    REQUIRE (tracks.size() == 2);
    CHECK (channelEvents (tracks[1]) == std::vector<Wire> {
        { 1200, 0x90, 60, 100 }, { 1201, 0x80, 60, 0 },
        { 1440, 0x90, 62, 100 }, { 1441, 0x80, 62, 0 },
    });
}

TEST_CASE ("A MIDI region that is not tempo-locked goes in at the session tempo, not the one it was recorded at",
           "[midi][recovery]")
{
    auto session = std::make_unique<Session>();
    session->tempoBpm.store (90.0f);

    MidiRegion region;
    region.timelineStart = 96030;
    region.recordedAtBPM = 90.0;
    region.lengthInTicks = 1920;
    region.lengthInSamples = ticksToSamples (1920, kRate, 90.0f);
    region.notes.push_back (note (1, 60, 100, 0, 480));
    region.notes.push_back (note (1, 64, 100, 480, 480));

    std::int64_t expectedStart = 0;
    std::int64_t startTick = 0;

    SECTION ("a floating region keeps its sample")
    {
        region.tempoLock = false;
        expectedStart = 96030;
        startTick = 1921;   // 96030 samples at 120 bpm, rounded; at the 90 it was recorded at, 1440
    }

    SECTION ("a tempo-locked region keeps its beat")
    {
        region.tempoLock = true;
        expectedStart = 72023;
        startTick = 1440;
    }

    session->track (0).midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (1, region));
    applyTempoChange (*session, 120.0f, kRate);
    const auto& regions = session->track (0).midiRegions.current();
    REQUIRE (regions.front().timelineStart == expectedStart);

    const auto file = recovery::buildPlayingFile (recovery::timingOf (*session, kRate), regions, "Keys");
    const auto tracks = tracksOf (file);
    REQUIRE (tracks.size() == 2);
    const auto events = channelEvents (tracks[1]);
    CHECK (events == std::vector<Wire> {
        { startTick, 0x90, 60, 100 },
        { startTick + 480, 0x80, 60, 0 },
        { startTick + 480, 0x90, 64, 100 },
        { startTick + 960, 0x80, 64, 0 },
    });
    CHECK (temposOf (file) == std::vector<Wire> { { 0, 500000, 0, 0 } });

    // Playback counts samples from the region's start; the file's ticks land
    // within half a tick (25 samples) of each.
    const auto played = playbackOf (regions, nullptr, 120.0f, 1000000);
    const auto filed = inSamples (events, nullptr, 120.0f);
    REQUIRE (filed.size() == played.size());
    for (std::size_t i = 0; i < played.size(); ++i)
    {
        CAPTURE (i, played[i], filed[i]);
        CHECK (filed[i].status == played[i].status);
        CHECK (filed[i].data1 == played[i].data1);
        CHECK (std::llabs (filed[i].at - played[i].at) <= 25);
    }
}

TEST_CASE ("A MIDI recovery file name keeps a track name to what every file system accepts",
           "[midi][recovery]")
{
    struct Case
    {
        std::string name;
        std::string sanitized;
    };
    const Case cases[] {
        { "Keys", "Keys" },
        { "AC/DC", "AC_DC" },
        { "a\\b:c*d?e\"f<g>h|i", "a_b_c_d_e_f_g_h_i" },
        { "tab\there\nnew", "tab_here_new" },
        { std::string ("nul\0byte", 8), "nul_byte" },
        { "del\x7f", "del_" },
        { "C1 \xc2\x85 control", "C1 _ control" },
        { "Caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xb9", "Caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xb9" },
        { "bad \xff byte", "bad _ byte" },
        { "cut \xc3", "cut _" },
        { "overlong \xc0\xaf", "overlong __" },
        { "surrogate \xed\xa0\x80", "surrogate ___" },
        { " Keys. .", " Keys" },
        { "Keys...", "Keys" },
        { "", "untitled" },
        { "...", "untitled" },
        { "   ", "untitled" },
    };
    for (const auto& c : cases)
    {
        CAPTURE (c.name);
        CHECK (recovery::sanitizeName (c.name) == c.sanitized);
    }
}

TEST_CASE ("A MIDI recovery file name stops a track name at 64 bytes without splitting a character",
           "[midi][recovery]")
{
    const std::string e ("\xc3\xa9");               // two bytes
    const std::string euro ("\xe2\x82\xac");        // three
    const std::string piano ("\xf0\x9f\x8e\xb9");   // four

    std::string twentyTwoEuros;
    for (int i = 0; i < 22; ++i)
        twentyTwoEuros += euro;

    CHECK (recovery::sanitizeName (std::string (64, 'a')) == std::string (64, 'a'));
    CHECK (recovery::sanitizeName (std::string (65, 'a')) == std::string (64, 'a'));
    CHECK (recovery::sanitizeName (std::string (62, 'a') + e) == std::string (62, 'a') + e);
    CHECK (recovery::sanitizeName (std::string (63, 'a') + e) == std::string (63, 'a'));
    CHECK (recovery::sanitizeName (std::string (60, 'a') + piano) == std::string (60, 'a') + piano);
    CHECK (recovery::sanitizeName (std::string (61, 'a') + piano) == std::string (61, 'a'));
    CHECK (recovery::sanitizeName (twentyTwoEuros) == twentyTwoEuros.substr (0, 63));
    CHECK (recovery::sanitizeName (std::string (62, 'a') + " " + e) == std::string (62, 'a'));
}

TEST_CASE ("A MIDI recovery file name gives the UTC second, the track number and the track name",
           "[midi][recovery]")
{
    const auto now = utc (kNovember8th143005);
    CHECK (recovery::fileName (now, 7, "Keys") == "20261108-143005Z_track07_Keys.mid");
    CHECK (recovery::fileName (now + std::chrono::milliseconds (999), 7, "Keys") == "20261108-143005Z_track07_Keys.mid");
    CHECK (recovery::fileName (now, 7, "Keys", recovery::TrackFile::Playing, 2) == "20261108-143005Z_track07_Keys_2.mid");
    CHECK (recovery::fileName (now, 12, "AC/DC") == "20261108-143005Z_track12_AC_DC.mid");
    CHECK (recovery::fileName (now, 1, "") == "20261108-143005Z_track01_untitled.mid");
    CHECK (recovery::fileName (utc (946684799), 24, "Keys") == "19991231-235959Z_track24_Keys.mid");
    CHECK (recovery::fileName (utc (951782400), 24, "Keys") == "20000229-000000Z_track24_Keys.mid");
    CHECK (recovery::fileName (utc (4107546123), 24, "Keys") == "21000301-010203Z_track24_Keys.mid");
}

TEST_CASE ("A MIDI takes file name ends .takes.mid, after the copy number", "[midi][recovery]")
{
    const auto now = utc (kNovember8th143005);
    const auto takes = recovery::TrackFile::Takes;
    CHECK (recovery::fileName (now, 7, "Keys", takes) == "20261108-143005Z_track07_Keys.takes.mid");
    CHECK (recovery::fileName (now, 7, "Keys", takes, 2) == "20261108-143005Z_track07_Keys_2.takes.mid");
    CHECK (recovery::fileName (now, 7, "Keys", takes, 1000) == "20261108-143005Z_track07_Keys_1000.takes.mid");
    CHECK (recovery::fileName (now, 12, "AC/DC", takes) == "20261108-143005Z_track12_AC_DC.takes.mid");
    CHECK (recovery::fileName (now, 1, "", takes) == "20261108-143005Z_track01_untitled.takes.mid");
}

TEST_CASE ("A MIDI take file made in the same second as another takes the next copy number",
           "[midi][recovery]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-recovery-copies-");
    auto session = std::make_unique<Session>();
    session->track (6).name = "Keys";
    const auto now = utc (kNovember8th143005);

    MidiRegion pass;
    std::vector<std::string> names;
    std::vector<Bytes> contents;
    for (int k = 0; k < 4; ++k)
    {
        pass.notes.assign (1, note (1, 60 + k, 100, 0, 480));
        const auto result = recovery::writeTakeFile (temp.path(), now + std::chrono::seconds (k / 3),
                                                     *session, kRate, 6, pass);
        REQUIRE (result.failures.empty());
        REQUIRE (result.written.size() == 1);
        CHECK (result.written.front().parent_path() == temp.path() / "midi" / "takes");
        names.push_back (result.written.front().filename().u8string());
        contents.push_back (recovery::buildTakeFile (recovery::timingOf (*session, kRate), pass, "Keys"));
    }

    CHECK (names == std::vector<std::string> {
        "20261108-143005Z_track07_Keys.mid",
        "20261108-143005Z_track07_Keys_2.mid",
        "20261108-143005Z_track07_Keys_3.mid",
        "20261108-143006Z_track07_Keys.mid",
    });
    CHECK (namesIn (temp.path() / "midi" / "takes") == names);
    for (std::size_t k = 0; k < names.size(); ++k)
    {
        CAPTURE (names[k]);
        CHECK (contentOf (temp.path() / "midi" / "takes" / names[k]) == contents[k]);
    }
}

TEST_CASE ("A MIDI take file is not written outside a session folder that does not exist",
           "[midi][recovery]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-recovery-missing-");
    auto session = std::make_unique<Session>();
    MidiRegion pass;
    pass.notes.push_back (note (1, 60, 100, 0, 480));

    const auto missing = temp.path() / "gone";
    const auto result = recovery::writeTakeFile (missing, utc (kNovember8th143005), *session, kRate, 0, pass);
    CHECK (result.written.empty());
    REQUIRE (result.failures.size() == 1);
    CHECK (result.failures.front().path == missing);
    CHECK (result.failures.front().error);
    CHECK_FALSE (stdfs::exists (missing));
}

TEST_CASE ("A MIDI track's playing file needs a region that plays, and its takes file a muted region or a previous take",
           "[midi][recovery]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-recovery-kinds-");
    const auto folder = temp.path() / "midi" / "tracks";
    auto session = std::make_unique<Session>();
    MidiRegion region;
    region.notes.push_back (note (1, 60, 100, 0, 480));
    session->track (2).name = "Bass";
    session->track (2).midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (1, region));
    region.muted = true;
    session->track (4).name = "Pads";
    session->track (4).midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (2, region));

    const auto result = recovery::writeChangedTrackFiles (temp.path(), utc (kNovember8th143005), *session, kRate);
    REQUIRE (result.failures.empty());
    REQUIRE (result.written == std::vector<stdfs::path> {
        folder / "20261108-143005Z_track03_Bass.mid",
        folder / "20261108-143005Z_track05_Pads.takes.mid",
    });
    CHECK (namesIn (folder) == std::vector<std::string> {
        "20261108-143005Z_track03_Bass.mid",
        "20261108-143005Z_track05_Pads.takes.mid",
    });

    const auto timing = recovery::timingOf (*session, kRate);
    CHECK (contentOf (result.written[0])
           == recovery::buildPlayingFile (timing, session->track (2).midiRegions.current(), "Bass"));
    CHECK (contentOf (result.written[1])
           == recovery::buildTakesFile (timing, session->track (4).midiRegions.current(), "Pads"));
}

TEST_CASE ("MIDI playing and takes files are each written only when they change", "[midi][recovery]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-recovery-changes-");
    const auto folder = temp.path() / "midi" / "tracks";
    auto session = std::make_unique<Session>();
    session->track (6).name = "Keys";
    auto& snapshot = session->track (6).midiRegions;

    MidiRegion verse;
    verse.timelineStart = 48000;
    verse.notes.push_back (note (1, 60, 100, 0, 480));
    verse.previousTakes.resize (1);
    verse.previousTakes.front().notes.push_back (note (1, 62, 100, 0, 480));
    MidiRegion chorus;
    chorus.timelineStart = 144000;
    chorus.notes.push_back (note (1, 64, 100, 0, 480));
    snapshot.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { verse, chorus }));

    // The names a pass writes, sorted; files keeps each file written with what
    // it held then.
    using Names = std::vector<std::string>;
    std::vector<std::pair<stdfs::path, Bytes>> files;
    const auto writeAt = [&] (int minute)
    {
        const auto result = recovery::writeChangedTrackFiles (
            temp.path(), utc (kNovember8th143005) + std::chrono::minutes (minute), *session, kRate);
        REQUIRE (result.failures.empty());
        Names names;
        for (const auto& path : result.written)
        {
            names.push_back (path.filename().u8string());
            files.emplace_back (path, contentOf (path));
        }
        std::sort (names.begin(), names.end());
        return names;
    };

    CHECK (writeAt (0) == Names { "20261108-143005Z_track07_Keys.mid", "20261108-143005Z_track07_Keys.takes.mid" });
    CHECK (writeAt (1).empty());

    // The chorus has no previous take, so only the playing file changes. Then
    // nothing is written, though the newest file there is a playing file.
    snapshot.mutate ([] (std::vector<MidiRegion>& regions) { regions.back().notes.front().velocity = 64; });
    CHECK (writeAt (2) == Names { "20261108-143205Z_track07_Keys.mid" });
    CHECK (writeAt (3).empty());

    // A previous take changes only the takes file. Then nothing is written,
    // though the newest file there is a takes file.
    snapshot.mutate ([] (std::vector<MidiRegion>& regions)
                     { regions.front().previousTakes.front().notes.front().velocity = 64; });
    CHECK (writeAt (4) == Names { "20261108-143405Z_track07_Keys.takes.mid" });
    CHECK (writeAt (5).empty());

    // Muting the chorus moves it from the playing file to the takes file.
    snapshot.mutatePlacements ([] (std::vector<MidiRegion>& regions) { regions.back().muted = true; });
    CHECK (writeAt (6) == Names { "20261108-143605Z_track07_Keys.mid", "20261108-143605Z_track07_Keys.takes.mid" });
    CHECK (writeAt (7).empty());
    CHECK (trackNames (contentOf (folder / "20261108-143605Z_track07_Keys.mid")) == Names { "Keys", "Region 1" });
    CHECK (trackNames (contentOf (folder / "20261108-143605Z_track07_Keys.takes.mid"))
           == Names { "Keys", "Region 1, take 2", "Region 2 (muted)" });

    for (const auto& file : files)
    {
        CAPTURE (file.first);
        CHECK (contentOf (file.first) == file.second);
    }
    CHECK (namesIn (folder) == Names {
        "20261108-143005Z_track07_Keys.mid",
        "20261108-143005Z_track07_Keys.takes.mid",
        "20261108-143205Z_track07_Keys.mid",
        "20261108-143405Z_track07_Keys.takes.mid",
        "20261108-143605Z_track07_Keys.mid",
        "20261108-143605Z_track07_Keys.takes.mid",
    });
}

TEST_CASE ("A MIDI takes file that changes again within a second takes the next copy number",
           "[midi][recovery]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-recovery-takes-copies-");
    auto session = std::make_unique<Session>();
    session->track (6).name = "Keys";
    auto& snapshot = session->track (6).midiRegions;
    MidiRegion region;
    region.notes.push_back (note (1, 60, 100, 0, 480));
    region.previousTakes.resize (1);
    snapshot.publish (std::make_unique<std::vector<MidiRegion>> (1, region));
    const auto now = utc (kNovember8th143005);

    std::vector<std::string> names;
    for (int k = 0; k < 3; ++k)
    {
        snapshot.mutate ([k] (std::vector<MidiRegion>& regions)
                         { regions.front().previousTakes.front().notes.assign (1, note (1, 62 + k, 100, 0, 480)); });
        const auto result = recovery::writeChangedTrackFiles (temp.path(), now + std::chrono::milliseconds (300 * k),
                                                              *session, kRate);
        REQUIRE (result.failures.empty());
        for (const auto& path : result.written)
            names.push_back (path.filename().u8string());
    }
    const auto unchanged = recovery::writeChangedTrackFiles (temp.path(), now + std::chrono::milliseconds (900),
                                                             *session, kRate);
    CHECK (unchanged.failures.empty());
    CHECK (unchanged.written.empty());

    std::sort (names.begin(), names.end());
    CHECK (names == std::vector<std::string> {
        "20261108-143005Z_track07_Keys.mid",
        "20261108-143005Z_track07_Keys.takes.mid",
        "20261108-143005Z_track07_Keys_2.takes.mid",
        "20261108-143005Z_track07_Keys_3.takes.mid",
    });
    CHECK (namesIn (temp.path() / "midi" / "tracks") == names);
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/midi/MidiFileReader.h"
#include "engine/midi/MidiFileWriter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

using duskstudio::midi::MidiFileConductor;
using duskstudio::midi::MidiFileEvent;
using duskstudio::midi::MidiFileReader;
using duskstudio::midi::MidiFileTrack;
using duskstudio::midi::writeMidiFileData;
using Catch::Matchers::WithinAbs;

namespace
{
using Bytes = std::vector<std::uint8_t>;

// Catch2 prints bytes above 0x7F raw, so byte comparisons go through this.
std::string toHex (const Bytes& bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto b : bytes)
    {
        if (! out.empty()) out += ' ';
        out += digits[b >> 4];
        out += digits[b & 0x0f];
    }
    return out;
}

MidiFileEvent event (std::int64_t tick, int status, int data1, int data2 = 0)
{
    MidiFileEvent ev;
    ev.tick   = tick;
    ev.status = (std::uint8_t) status;
    ev.data1  = (std::uint8_t) data1;
    ev.data2  = (std::uint8_t) data2;
    return ev;
}

// One event as it sits in the file: channel messages by their bytes, meta events
// by their type byte, the way MidiFileReader reports them.
struct Wire
{
    std::int64_t tick;
    int status;
    int data1;
    int data2;

    bool operator== (const Wire& o) const noexcept
    {
        return tick == o.tick && status == o.status && data1 == o.data1 && data2 == o.data2;
    }
};

std::ostream& operator<< (std::ostream& os, const Wire& w)
{
    return os << "{ " << w.tick << ", 0x" << std::hex << w.status << std::dec
              << ", " << w.data1 << ", " << w.data2 << " }";
}

Wire meta (std::int64_t tick, int type) { return { tick, 0xff, type, 0 }; }

std::vector<Wire> wires (const std::vector<MidiFileEvent>& events)
{
    std::vector<Wire> out;
    for (const auto& ev : events)
        out.push_back ({ ev.tick, ev.status, ev.data1, ev.data2 });
    return out;
}

std::vector<Wire> juceChannelEvents (const juce::MidiMessageSequence& sequence)
{
    std::vector<Wire> out;
    for (int i = 0; i < sequence.getNumEvents(); ++i)
    {
        const auto& message = sequence.getEventPointer (i)->message;
        const auto* raw = message.getRawData();
        const int size = message.getRawDataSize();
        if (raw[0] < 0x80 || raw[0] >= 0xf0) continue;
        out.push_back ({ (std::int64_t) std::llround (message.getTimeStamp()), raw[0],
                         size > 1 ? raw[1] : 0, size > 2 ? raw[2] : 0 });
    }
    return out;
}

std::string headerOf (const Bytes& file)
{
    REQUIRE (file.size() >= 14);
    return toHex (Bytes (file.begin(), file.begin() + 14));
}

// Every MTrk body in file order; fails unless the chunks fill the file exactly.
std::vector<Bytes> trackBodies (const Bytes& file)
{
    REQUIRE (file.size() >= 14);
    std::vector<Bytes> bodies;
    std::size_t pos = 14;
    while (pos < file.size())
    {
        REQUIRE (file.size() - pos >= 8);
        const auto chunk = file.begin() + (std::ptrdiff_t) pos;
        REQUIRE (toHex (Bytes (chunk, chunk + 4)) == toHex ({ 'M', 'T', 'r', 'k' }));
        const std::size_t length = ((std::size_t) chunk[4] << 24) | ((std::size_t) chunk[5] << 16)
                                 | ((std::size_t) chunk[6] << 8)  | (std::size_t) chunk[7];
        REQUIRE (file.size() - pos - 8 >= length);
        bodies.emplace_back (chunk + 8, chunk + 8 + (std::ptrdiff_t) length);
        pos += 8 + length;
    }
    return bodies;
}

std::string onlyTrackBody (const MidiFileTrack& track)
{
    const auto bodies = trackBodies (writeMidiFileData (480, {}, { track }));
    REQUIRE (bodies.size() == 2);
    return toHex (bodies[1]);
}

struct Song
{
    MidiFileConductor conductor;
    std::vector<MidiFileTrack> tracks;
};

// Events in the order the writer puts them, so expected output equals input.
Song sampleSong()
{
    Song song;
    song.conductor.name = "Song";
    song.conductor.timeSignatureNumerator   = 7;
    song.conductor.timeSignatureDenominator = 8;
    song.conductor.tempos = { { 0, 500000 }, { 960, 428571 }, { 2880, 600000 } };

    const MidiFileTrack keys { "Caf\xc3\xa9 Keys", {
        event (0,   0xc0, 5),
        event (0,   0x90, 60, 100),
        event (0,   0x91, 64, 90),
        event (240, 0xe0, 0x00, 0x48),
        event (360, 0xd0, 70),
        event (400, 0xa0, 60, 50),
        event (480, 0x80, 60, 64),
        event (480, 0xb0, 64, 127),
        event (480, 0x90, 60, 80),
        event (720, 0x91, 64, 0),
        event (960, 0x80, 60, 0),
        event (960, 0xbf, 7, 90),
    } };
    const MidiFileTrack bass { "Bass", { event (240, 0x92, 36, 110), event (1680, 0x82, 36, 0) } };

    song.tracks = { keys, bass };
    return song;
}
} // namespace

TEST_CASE ("MidiFileWriter writes a format 1 header that counts the conductor track", "[midi][writer]")
{
    SECTION ("conductor only")
    {
        const auto file = writeMidiFileData (480, {}, {});
        CHECK (headerOf (file) == toHex ({ 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 1, 0x01, 0xe0 }));
        CHECK (trackBodies (file).size() == 1);
    }

    SECTION ("two note tracks at 96 ticks per quarter")
    {
        const auto file = writeMidiFileData (96, {}, { MidiFileTrack {}, MidiFileTrack {} });
        CHECK (headerOf (file) == toHex ({ 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 3, 0x00, 0x60 }));
        CHECK (trackBodies (file).size() == 3);
    }
}

TEST_CASE ("MidiFileWriter writes the conductor track byte for byte", "[midi][writer]")
{
    MidiFileConductor conductor;
    conductor.name = "Song";
    conductor.timeSignatureNumerator   = 6;
    conductor.timeSignatureDenominator = 8;

    const auto expected = toHex ({
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x01, 0xe0,
        'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x23,
        0x00, 0xff, 0x03, 0x04, 'S', 'o', 'n', 'g',
        0x00, 0xff, 0x58, 0x04, 0x06, 0x03, 0x18, 0x08,
        0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20,
        0x8f, 0x00, 0xff, 0x51, 0x03, 0x06, 0x1a, 0x80,
        0x00, 0xff, 0x2f, 0x00 });

    SECTION ("tempo changes in tick order")
    {
        conductor.tempos = { { 0, 500000 }, { 1920, 400000 } };
        CHECK (toHex (writeMidiFileData (480, conductor, {})) == expected);
    }

    SECTION ("tempo changes out of tick order")
    {
        conductor.tempos = { { 1920, 400000 }, { 0, 500000 } };
        CHECK (toHex (writeMidiFileData (480, conductor, {})) == expected);
    }
}

TEST_CASE ("MidiFileWriter encodes delta times as variable-length quantities", "[midi][writer]")
{
    struct Case
    {
        std::int64_t delta;
        Bytes encoded;
    };
    const Case cases[] {
        { 0,          { 0x00 } },
        { 127,        { 0x7f } },
        { 128,        { 0x81, 0x00 } },
        { 16383,      { 0xff, 0x7f } },
        { 16384,      { 0x81, 0x80, 0x00 } },
        { 0x0fffffff, { 0xff, 0xff, 0xff, 0x7f } },
    };

    // Two controllers, one category, so equal ticks keep this order. The first
    // sits at tick 1000 (0x87 0x68) to show deltas are relative.
    for (const auto& c : cases)
    {
        CAPTURE (c.delta);
        const MidiFileTrack track { "", { event (1000, 0xb0, 1, 10), event (1000 + c.delta, 0xb0, 2, 20) } };

        Bytes expected { 0x87, 0x68, 0xb0, 0x01, 0x0a };
        expected.insert (expected.end(), c.encoded.begin(), c.encoded.end());
        expected.insert (expected.end(), { 0xb0, 0x02, 0x14, 0x00, 0xff, 0x2f, 0x00 });
        CHECK (onlyTrackBody (track) == toHex (expected));
    }

    const MidiFileTrack fromPrevious { "", { event (1000, 0xb0, 1, 10),
                                             event (1000 + 0x10000000, 0xb0, 2, 20) } };
    CHECK (writeMidiFileData (480, {}, { fromPrevious }).empty());

    const MidiFileTrack fromTrackStart { "", { event (0x10000000, 0xb0, 1, 10) } };
    CHECK (writeMidiFileData (480, {}, { fromTrackStart }).empty());
}

TEST_CASE ("MidiFileWriter writes one data byte for program change and channel pressure",
           "[midi][writer]")
{
    const MidiFileTrack track { "", { event (0, 0xc3, 12, 99), event (0, 0xd3, 80, 0xff) } };
    CHECK (onlyTrackBody (track) == toHex ({ 0x00, 0xc3, 0x0c, 0x00, 0xd3, 0x50, 0x00, 0xff, 0x2f, 0x00 }));
}

TEST_CASE ("MidiFileWriter round-trips every channel voice message through MidiFileReader",
           "[midi][writer]")
{
    const auto song = sampleSong();
    const auto file = writeMidiFileData (480, song.conductor, song.tracks);
    REQUIRE_FALSE (file.empty());

    MidiFileReader reader;
    REQUIRE (reader.readData (file.data(), file.size()));
    REQUIRE (reader.timeFormat() == 480);
    REQUIRE (reader.tracks().size() == 3);

    CHECK (wires (reader.tracks()[0]) == std::vector<Wire> {
        meta (0, 0x03), meta (0, 0x58), meta (0, 0x51), meta (960, 0x51), meta (2880, 0x51),
        meta (2880, 0x2f) });

    for (std::size_t t = 0; t < song.tracks.size(); ++t)
    {
        CAPTURE (t);
        const auto& events = song.tracks[t].events;
        auto expected = wires (events);
        expected.insert (expected.begin(), meta (0, 0x03));
        expected.push_back (meta (events.back().tick, 0x2f));
        CHECK (wires (reader.tracks()[t + 1]) == expected);
    }
}

TEST_CASE ("MidiFileWriter writes equal-tick note-offs, then controls, then note-ons, then poly pressure",
           "[midi][writer]")
{
    SECTION ("a note that ends where the next one of its pitch starts, given in any order")
    {
        const MidiFileEvent atBoundary[] {
            event (480, 0x90, 60, 90), event (480, 0xb0, 64, 127), event (480, 0x80, 60, 0) };
        const auto expected = toHex ({
            0x00, 0x90, 0x3c, 0x64,
            0x83, 0x60, 0x80, 0x3c, 0x00,
            0x00, 0xb0, 0x40, 0x7f,
            0x00, 0x90, 0x3c, 0x5a,
            0x83, 0x60, 0x80, 0x3c, 0x00,
            0x00, 0xff, 0x2f, 0x00 });

        std::vector<std::size_t> order { 0, 1, 2 };
        do
        {
            CAPTURE (order);
            MidiFileTrack track;
            track.events.push_back (event (0, 0x90, 60, 100));
            for (const auto i : order)
                track.events.push_back (atBoundary[i]);
            track.events.push_back (event (960, 0x80, 60, 0));
            CHECK (onlyTrackBody (track) == expected);
        }
        while (std::next_permutation (order.begin(), order.end()));
    }

    SECTION ("both note-off forms lead, channel controls precede the note-ons and poly pressure follows them")
    {
        const MidiFileTrack track { "", {
            event (0, 0xa0, 62, 32),
            event (0, 0x90, 62, 100),
            event (0, 0xe0, 0x00, 0x40),
            event (0, 0x90, 61, 0),
            event (0, 0xc0, 5),
            event (0, 0xb0, 7, 100),
            event (0, 0xd0, 48),
            event (0, 0x80, 60, 0),
        } };
        CHECK (onlyTrackBody (track) == toHex ({
            0x00, 0x90, 0x3d, 0x00,
            0x00, 0x80, 0x3c, 0x00,
            0x00, 0xe0, 0x00, 0x40,
            0x00, 0xc0, 0x05,
            0x00, 0xb0, 0x07, 0x64,
            0x00, 0xd0, 0x30,
            0x00, 0x90, 0x3e, 0x64,
            0x00, 0xa0, 0x3e, 0x20,
            0x00, 0xff, 0x2f, 0x00 }));
    }

    SECTION ("events of one kind keep the caller's order")
    {
        const MidiFileTrack track { "", {
            event (0, 0x90, 64, 100),
            event (0, 0xb0, 7, 100),
            event (0, 0x90, 67, 0),
            event (0, 0x90, 62, 100),
            event (0, 0xb0, 1, 20),
            event (0, 0x80, 65, 0),
        } };
        CHECK (onlyTrackBody (track) == toHex ({
            0x00, 0x90, 0x43, 0x00,
            0x00, 0x80, 0x41, 0x00,
            0x00, 0xb0, 0x07, 0x64,
            0x00, 0xb0, 0x01, 0x14,
            0x00, 0x90, 0x40, 0x64,
            0x00, 0x90, 0x3e, 0x64,
            0x00, 0xff, 0x2f, 0x00 }));
    }

    SECTION ("events given out of tick order")
    {
        const MidiFileTrack track { "", {
            event (960, 0xb0, 7, 100), event (0, 0x90, 60, 100), event (480, 0x80, 60, 0) } };
        CHECK (onlyTrackBody (track) == toHex ({
            0x00, 0x90, 0x3c, 0x64,
            0x83, 0x60, 0x80, 0x3c, 0x00,
            0x83, 0x60, 0xb0, 0x07, 0x64,
            0x00, 0xff, 0x2f, 0x00 }));
    }
}

TEST_CASE ("MidiFileWriter output reads back the same through the JUCE MidiFile reader",
           "[midi][writer]")
{
    const auto song = sampleSong();
    const auto file = writeMidiFileData (480, song.conductor, song.tracks);
    REQUIRE_FALSE (file.empty());

    juce::MidiFile reference;
    juce::MemoryInputStream in (file.data(), file.size(), false);
    REQUIRE (reference.readFrom (in));
    REQUIRE (reference.getTimeFormat() == 480);
    REQUIRE (reference.getNumTracks() == 3);

    juce::MidiMessageSequence tempos;
    reference.findAllTempoEvents (tempos);
    REQUIRE (tempos.getNumEvents() == (int) song.conductor.tempos.size());
    for (int i = 0; i < tempos.getNumEvents(); ++i)
    {
        CAPTURE (i);
        const auto& message = tempos.getEventPointer (i)->message;
        const auto& expected = song.conductor.tempos[(std::size_t) i];
        CHECK (std::llround (message.getTimeStamp()) == expected.tick);
        CHECK_THAT (message.getTempoSecondsPerQuarterNote(),
                    WithinAbs (expected.microsecondsPerQuarter / 1.0e6, 1.0e-12));
    }

    juce::MidiMessageSequence meters;
    reference.findAllTimeSigEvents (meters);
    REQUIRE (meters.getNumEvents() == 1);
    const auto& meter = meters.getEventPointer (0)->message;
    int numerator = 0;
    int denominator = 0;
    meter.getTimeSignatureInfo (numerator, denominator);
    CHECK (std::llround (meter.getTimeStamp()) == 0);
    CHECK (numerator == 7);
    CHECK (denominator == 8);

    const std::string names[] { song.conductor.name, song.tracks[0].name, song.tracks[1].name };
    const std::int64_t ends[] { 2880, 960, 1680 };
    for (int t = 0; t < reference.getNumTracks(); ++t)
    {
        CAPTURE (t);
        const auto& sequence = *reference.getTrack (t);
        REQUIRE (sequence.getNumEvents() >= 2);

        const auto& first = sequence.getEventPointer (0)->message;
        REQUIRE (first.isTrackNameEvent());
        CHECK (first.getTextFromTextMetaEvent().toStdString() == names[t]);

        const auto& last = sequence.getEventPointer (sequence.getNumEvents() - 1)->message;
        CHECK (last.isEndOfTrackMetaEvent());
        CHECK (std::llround (last.getTimeStamp()) == ends[t]);
    }

    CHECK (juceChannelEvents (*reference.getTrack (0)).empty());
    for (std::size_t t = 0; t < song.tracks.size(); ++t)
    {
        CAPTURE (t);
        CHECK (juceChannelEvents (*reference.getTrack ((int) t + 1)) == wires (song.tracks[t].events));
    }
}

TEST_CASE ("MidiFileWriter refuses input it cannot write faithfully", "[midi][writer]")
{
    int ticksPerQuarter = 480;
    MidiFileConductor conductor;
    std::vector<MidiFileTrack> tracks { { "", { event (0, 0x90, 60, 100), event (480, 0x80, 60, 0) } } };
    REQUIRE_FALSE (writeMidiFileData (ticksPerQuarter, conductor, tracks).empty());

    SECTION ("zero ticks per quarter")                   { ticksPerQuarter = 0; }
    SECTION ("negative ticks per quarter")               { ticksPerQuarter = -480; }
    SECTION ("ticks per quarter above 0x7FFF")           { ticksPerQuarter = 0x8000; }
    SECTION ("a negative event tick")                    { tracks[0].events[0].tick = -1; }
    SECTION ("a tempo change before tick 0")             { conductor.tempos.push_back ({ -1, 400000 }); }
    SECTION ("a tempo delta above 0x0FFFFFFF")           { conductor.tempos.push_back ({ 0x10000000, 400000 }); }
    SECTION ("a status below 0x80")                      { tracks[0].events[0].status = 0x7f; }
    SECTION ("a system exclusive status")                { tracks[0].events[0].status = 0xf0; }
    SECTION ("a meta event status")                      { tracks[0].events[0].status = 0xff; }
    SECTION ("data1 above 0x7F")                         { tracks[0].events[0].data1 = 0x80; }
    SECTION ("data2 above 0x7F")                         { tracks[0].events[0].data2 = 0x80; }
    SECTION ("a time signature denominator of zero")     { conductor.timeSignatureDenominator = 0; }
    SECTION ("a denominator that is not a power of two") { conductor.timeSignatureDenominator = 3; }
    SECTION ("a denominator above 64")                   { conductor.timeSignatureDenominator = 128; }
    SECTION ("a numerator of zero")                      { conductor.timeSignatureNumerator = 0; }
    SECTION ("a numerator above 255")                    { conductor.timeSignatureNumerator = 256; }
    SECTION ("a tempo of zero")                          { conductor.tempos[0].microsecondsPerQuarter = 0; }
    SECTION ("a tempo above 0xFFFFFF")                   { conductor.tempos[0].microsecondsPerQuarter = 0x1000000; }
    SECTION ("a tempo map that starts after tick 0")     { conductor.tempos[0].tick = 480; }
    SECTION ("an empty tempo map")                       { conductor.tempos.clear(); }
    SECTION ("more tracks than the header can count")    { tracks.resize (0xffff); }

    CHECK (writeMidiFileData (ticksPerQuarter, conductor, tracks).empty());
}

TEST_CASE ("MidiFileWriter accepts every field at the edge of its range", "[midi][writer]")
{
    MidiFileConductor conductor;

    SECTION ("1 and 0x7FFF ticks per quarter")
    {
        CHECK (headerOf (writeMidiFileData (1, conductor, {}))
               == toHex ({ 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 1, 0x00, 0x01 }));
        CHECK (headerOf (writeMidiFileData (0x7fff, conductor, {}))
               == toHex ({ 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 1, 0x7f, 0xff }));
    }

    SECTION ("time signatures of 1/1 and 255/64")
    {
        conductor.timeSignatureNumerator   = 1;
        conductor.timeSignatureDenominator = 1;
        CHECK (toHex (trackBodies (writeMidiFileData (480, conductor, {}))[0]) == toHex ({
            0x00, 0xff, 0x58, 0x04, 0x01, 0x00, 0x18, 0x08,
            0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20,
            0x00, 0xff, 0x2f, 0x00 }));

        conductor.timeSignatureNumerator   = 255;
        conductor.timeSignatureDenominator = 64;
        CHECK (toHex (trackBodies (writeMidiFileData (480, conductor, {}))[0]) == toHex ({
            0x00, 0xff, 0x58, 0x04, 0xff, 0x06, 0x18, 0x08,
            0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20,
            0x00, 0xff, 0x2f, 0x00 }));
    }

    SECTION ("tempos of 1 and 0xFFFFFF microseconds per quarter")
    {
        conductor.tempos = { { 0, 1 }, { 1, 0xffffff } };
        CHECK (toHex (trackBodies (writeMidiFileData (480, conductor, {}))[0]) == toHex ({
            0x00, 0xff, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08,
            0x00, 0xff, 0x51, 0x03, 0x00, 0x00, 0x01,
            0x01, 0xff, 0x51, 0x03, 0xff, 0xff, 0xff,
            0x00, 0xff, 0x2f, 0x00 }));
    }

    SECTION ("statuses 0x80 and 0xEF carrying 0x7F data")
    {
        const MidiFileTrack track { "", { event (0, 0x80, 0x7f, 0x7f), event (0, 0xef, 0x7f, 0x7f) } };
        CHECK (onlyTrackBody (track) == toHex ({
            0x00, 0x80, 0x7f, 0x7f, 0x00, 0xef, 0x7f, 0x7f, 0x00, 0xff, 0x2f, 0x00 }));
    }

    SECTION ("0xFFFE note tracks with the conductor")
    {
        const auto file = writeMidiFileData (480, conductor, std::vector<MidiFileTrack> (0xfffe));
        CHECK (headerOf (file) == toHex ({ 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0xff, 0xff, 0x01, 0xe0 }));
        CHECK (trackBodies (file).size() == 0xffffu);
    }
}

TEST_CASE ("MidiFileWriter ends an empty note track with end of track at tick 0", "[midi][writer]")
{
    MidiFileTrack track;
    Bytes expectedBody;
    std::vector<Wire> expectedEvents;

    SECTION ("without a name")
    {
        expectedBody   = { 0x00, 0xff, 0x2f, 0x00 };
        expectedEvents = { meta (0, 0x2f) };
    }

    SECTION ("with a name")
    {
        track.name     = "Pad";
        expectedBody   = { 0x00, 0xff, 0x03, 0x03, 'P', 'a', 'd', 0x00, 0xff, 0x2f, 0x00 };
        expectedEvents = { meta (0, 0x03), meta (0, 0x2f) };
    }

    const auto file = writeMidiFileData (480, {}, { track });
    CHECK (onlyTrackBody (track) == toHex (expectedBody));

    MidiFileReader reader;
    REQUIRE (reader.readData (file.data(), file.size()));
    REQUIRE (reader.tracks().size() == 2);
    CHECK (wires (reader.tracks()[1]) == expectedEvents);
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/midi/MidiFileReader.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using duskstudio::midi::MidiFileReader;
using Catch::Matchers::WithinAbs;

namespace
{
std::vector<std::uint8_t> loadHexFixture (const char* name)
{
    const auto path = std::string (DUSKSTUDIO_SOURCE_DIR)
                    + "/tests/fixtures/midi/" + name;
    std::ifstream input (path);
    std::vector<std::uint8_t> bytes;
    std::string token;
    while (input >> token)
        bytes.push_back ((std::uint8_t) std::stoul (token, nullptr, 16));
    return bytes;
}

struct SmfWriter
{
    std::vector<std::uint8_t> bytes;

    void u8 (int v)                       { bytes.push_back ((std::uint8_t) v); }
    void u16 (int v)                      { u8 ((v >> 8) & 0xff); u8 (v & 0xff); }
    void u32 (std::uint32_t v)            { u8 ((int) (v >> 24)); u8 ((int) (v >> 16));
                                            u8 ((int) (v >> 8)); u8 ((int) v); }
    void tag (const char* s)              { for (int i = 0; i < 4; ++i) u8 (s[i]); }

    void varLen (std::uint32_t v)
    {
        std::uint32_t buffer = v & 0x7f;
        while ((v >>= 7) != 0)
        {
            buffer <<= 8;
            buffer |= ((v & 0x7f) | 0x80);
        }
        for (;;)
        {
            u8 ((int) (buffer & 0xff));
            if (buffer & 0x80) buffer >>= 8;
            else break;
        }
    }

    void header (int fileType, int numTracks, int timeFormat)
    {
        tag ("MThd");
        u32 (6);
        u16 (fileType);
        u16 (numTracks);
        u16 (timeFormat & 0xffff);
    }

    void track (const std::vector<std::uint8_t>& body)
    {
        tag ("MTrk");
        u32 ((std::uint32_t) body.size());
        bytes.insert (bytes.end(), body.begin(), body.end());
    }
};

struct TrackWriter
{
    std::vector<std::uint8_t> bytes;
    std::uint32_t lastTick = 0;

    void u8 (int v) { bytes.push_back ((std::uint8_t) v); }

    void delta (std::uint32_t tick)
    {
        SmfWriter tmp;
        tmp.varLen (tick - lastTick);
        bytes.insert (bytes.end(), tmp.bytes.begin(), tmp.bytes.end());
        lastTick = tick;
    }

    void event (std::uint32_t tick, int status, int d1, int d2)
    {
        delta (tick); u8 (status); u8 (d1); u8 (d2);
    }

    // Emits the data bytes only, reusing the status byte of the previous event.
    void running (std::uint32_t tick, int d1, int d2)
    {
        delta (tick); u8 (d1); u8 (d2);
    }

    void meta (std::uint32_t tick, int type, const std::vector<std::uint8_t>& payload)
    {
        delta (tick); u8 (0xff); u8 (type);
        SmfWriter tmp;
        tmp.varLen ((std::uint32_t) payload.size());
        bytes.insert (bytes.end(), tmp.bytes.begin(), tmp.bytes.end());
        bytes.insert (bytes.end(), payload.begin(), payload.end());
    }

    void endOfTrack (std::uint32_t tick) { meta (tick, 0x2f, {}); }
};

struct FlatEvent
{
    std::int64_t tick;
    int status;
    int data1;
    int data2;
    bool channelMessage;

    bool operator== (const FlatEvent& o) const
    {
        if (tick != o.tick || status != o.status || channelMessage != o.channelMessage)
            return false;
        if (status == 0xff)
            return data1 == o.data1;          // meta type; payload is not kept
        return ! channelMessage || (data1 == o.data1 && data2 == o.data2);
    }
};

std::vector<FlatEvent> flatten (const MidiFileReader& reader)
{
    std::vector<FlatEvent> out;
    for (const auto& track : reader.tracks())
        for (const auto& ev : track)
            out.push_back ({ ev.tick, ev.status, ev.data1, ev.data2, ev.isChannelMessage() });
    return out;
}

std::vector<FlatEvent> flattenJuce (const juce::MidiFile& file)
{
    std::vector<FlatEvent> out;
    for (int t = 0; t < file.getNumTracks(); ++t)
    {
        const auto* seq = file.getTrack (t);
        if (seq == nullptr) continue;

        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& m = seq->getEventPointer (i)->message;
            const auto* raw = m.getRawData();
            const int   size = m.getRawDataSize();
            const bool  channelMessage = (raw[0] >= 0x80 && raw[0] < 0xf0);
            out.push_back ({ (std::int64_t) std::llround (m.getTimeStamp()),
                             raw[0],
                             size > 1 ? raw[1] : 0,
                             size > 2 ? raw[2] : 0,
                             channelMessage });
        }
    }
    return out;
}

void requireMatchesJuce (const std::vector<std::uint8_t>& file)
{
    MidiFileReader dut;
    REQUIRE (dut.readData (file.data(), file.size()));

    juce::MidiFile reference;
    juce::MemoryInputStream in (file.data(), file.size(), false);
    REQUIRE (reference.readFrom (in));

    REQUIRE (dut.timeFormat() == (int) reference.getTimeFormat());

    const auto dutEvents = flatten (dut);
    const auto refEvents = flattenJuce (reference);
    REQUIRE (dutEvents.size() == refEvents.size());
    for (size_t i = 0; i < dutEvents.size(); ++i)
        REQUIRE (dutEvents[i] == refEvents[i]);
}

std::vector<std::uint8_t> simpleType0()
{
    TrackWriter t;
    t.event (0,   0x90, 60, 100);
    t.event (48,  0xb0, 7, 90);
    t.event (96,  0x80, 60, 64);
    t.endOfTrack (192);

    SmfWriter f;
    f.header (0, 1, 96);
    f.track (t.bytes);
    return f.bytes;
}

std::vector<std::uint8_t> type1WithTempoAndRunningStatus()
{
    TrackWriter tempo;
    tempo.meta (0, 0x51, { 0x07, 0xa1, 0x20 });     // 500000 us per quarter
    tempo.meta (0, 0x58, { 4, 2, 24, 8 });          // 4/4
    tempo.meta (960, 0x51, { 0x05, 0x16, 0x15 });   // tempo change part-way
    tempo.endOfTrack (1920);

    TrackWriter notes;
    notes.event   (0,   0x91, 64, 80);
    notes.running (240, 67, 80);                    // running status note-on
    notes.running (480, 64, 0);                     // running status note-off
    notes.event   (720, 0x81, 67, 0);
    notes.endOfTrack (1920);

    SmfWriter f;
    f.header (1, 2, 480);
    f.track (tempo.bytes);
    f.track (notes.bytes);
    return f.bytes;
}
} // namespace

TEST_CASE ("MidiFileReader parses a type 0 file", "[midi][import]")
{
    MidiFileReader reader;
    const auto file = simpleType0();
    REQUIRE (reader.readData (file.data(), file.size()));

    REQUIRE (reader.timeFormat() == 96);
    REQUIRE_FALSE (reader.isSmpteTimeFormat());
    REQUIRE (reader.tracks().size() == 1);

    const auto& events = reader.tracks()[0];
    REQUIRE (events.size() == 4);

    REQUIRE (events[0].isNoteOn());
    REQUIRE (events[0].tick == 0);
    REQUIRE (events[0].channel() == 1);
    REQUIRE (events[0].noteNumber() == 60);
    REQUIRE (events[0].velocity() == 100);

    REQUIRE (events[1].isController());
    REQUIRE (events[1].tick == 48);
    REQUIRE (events[1].controllerNumber() == 7);
    REQUIRE (events[1].controllerValue() == 90);

    REQUIRE (events[2].isNoteOff());
    REQUIRE (events[2].tick == 96);

    REQUIRE (events[3].tick == 192);
    REQUIRE_FALSE (events[3].isChannelMessage());
    REQUIRE (events[3].channel() == 0);
}

TEST_CASE ("MidiFileReader parses a type 1 file with tempo changes and running status",
           "[midi][import]")
{
    MidiFileReader reader;
    const auto file = type1WithTempoAndRunningStatus();
    REQUIRE (reader.readData (file.data(), file.size()));

    REQUIRE (reader.timeFormat() == 480);
    REQUIRE (reader.tracks().size() == 2);
    REQUIRE (reader.tracks()[0].size() == 4);

    const auto& notes = reader.tracks()[1];
    REQUIRE (notes.size() == 5);
    REQUIRE (notes[1].isNoteOn());
    REQUIRE (notes[1].status == 0x91);
    REQUIRE (notes[1].noteNumber() == 67);
    REQUIRE (notes[2].isNoteOff());
    REQUIRE (notes[2].status == 0x91);
    REQUIRE (notes[2].noteNumber() == 64);
    REQUIRE (notes[2].channel() == 2);
}

TEST_CASE ("MidiFileReader matches the JUCE reader event for event", "[midi][import]")
{
    requireMatchesJuce (simpleType0());
    requireMatchesJuce (type1WithTempoAndRunningStatus());
}

TEST_CASE ("MidiFileReader closes a retriggered note before it sounds again", "[midi][import]")
{
    TrackWriter t;
    t.event (0,   0x90, 60, 100);
    t.event (100, 0x90, 60, 90);
    t.event (200, 0x80, 60, 0);
    t.endOfTrack (240);

    SmfWriter f;
    f.header (0, 1, 96);
    f.track (t.bytes);

    MidiFileReader reader;
    REQUIRE (reader.readData (f.bytes.data(), f.bytes.size()));

    const auto& events = reader.tracks()[0];
    REQUIRE (events.size() == 5);
    REQUIRE (events[1].isNoteOff());
    REQUIRE (events[1].tick == 100);
    REQUIRE (events[1].noteNumber() == 60);
    REQUIRE (events[1].channel() == 1);
    REQUIRE (events[2].isNoteOn());
    REQUIRE (events[2].tick == 100);

    requireMatchesJuce (f.bytes);
}

TEST_CASE ("MidiFileReader sorts a note-off ahead of a note-on sharing its tick",
           "[midi][import]")
{
    TrackWriter t;
    t.event (0,   0x90, 60, 100);
    t.event (100, 0x90, 60, 90);   // re-strike written before the release
    t.event (100, 0x80, 60, 0);
    t.event (200, 0x80, 60, 0);
    t.endOfTrack (240);

    SmfWriter f;
    f.header (0, 1, 96);
    f.track (t.bytes);

    MidiFileReader reader;
    REQUIRE (reader.readData (f.bytes.data(), f.bytes.size()));

    const auto& events = reader.tracks()[0];
    REQUIRE (events[1].tick == 100);
    REQUIRE (events[1].isNoteOff());
    REQUIRE (events[2].tick == 100);
    REQUIRE (events[2].isNoteOn());

    requireMatchesJuce (f.bytes);
}

TEST_CASE ("MidiFileReader reports SMPTE tick rates", "[midi][import]")
{
    TrackWriter t;
    t.event (0, 0x90, 60, 100);
    t.event (1000, 0x80, 60, 0);
    t.endOfTrack (2000);

    SmfWriter f;
    f.header (0, 1, (int) (std::uint16_t) 0xe728);   // 25 fps, 40 subframes
    f.track (t.bytes);

    MidiFileReader reader;
    REQUIRE (reader.readData (f.bytes.data(), f.bytes.size()));

    REQUIRE (reader.isSmpteTimeFormat());
    REQUIRE_THAT (reader.smpteTicksPerSecond(), WithinAbs (1000.0, 1.0e-9));
    REQUIRE (reader.tracks()[0][1].tick == 1000);
}

TEST_CASE ("MidiFileReader rejects files it cannot trust", "[midi][import]")
{
    MidiFileReader reader;

    REQUIRE_FALSE (reader.readData (nullptr, 0));

    const std::vector<std::uint8_t> notMidi { 'J', 'U', 'N', 'K', 0, 0, 0, 6 };
    REQUIRE_FALSE (reader.readData (notMidi.data(), notMidi.size()));

    auto truncated = simpleType0();
    truncated.resize (truncated.size() - 4);
    REQUIRE_FALSE (reader.readData (truncated.data(), truncated.size()));

    SmfWriter headerOnly;
    headerOnly.header (0, 3, 96);   // type 0 must declare exactly one track
    REQUIRE_FALSE (reader.readData (headerOnly.bytes.data(), headerOnly.bytes.size()));

    REQUIRE (reader.tracks().empty());
}

TEST_CASE ("MidiFileReader drops an event truncated by the end of its track", "[midi][import]")
{
    TrackWriter t;
    t.event (0, 0x90, 60, 100);
    t.delta (10);
    t.u8 (0x90);
    t.u8 (62);              // velocity byte missing

    SmfWriter f;
    f.header (0, 1, 96);
    f.track (t.bytes);

    MidiFileReader reader;
    REQUIRE (reader.readData (f.bytes.data(), f.bytes.size()));
    REQUIRE (reader.tracks().size() == 1);
    REQUIRE (reader.tracks()[0].size() == 1);
}

TEST_CASE ("MidiFileReader skips vendor chunks without consuming the track count",
           "[midi][import][regression][issue-463]")
{
    const auto file = loadHexFixture ("vendor-chunk.mid.hex");
    REQUIRE_FALSE (file.empty());

    MidiFileReader reader;
    REQUIRE (reader.readData (file.data(), file.size()));
    REQUIRE (reader.tracks().size() == 2);
    REQUIRE (reader.tracks()[0].size() == 2);
    REQUIRE (reader.tracks()[1].size() == 2);
    REQUIRE (reader.tracks()[0][0].noteNumber() == 60);
    REQUIRE (reader.tracks()[1][0].noteNumber() == 64);
}

TEST_CASE ("MidiFileReader keeps parsing after an in-track realtime message",
           "[midi][import][regression][issue-463]")
{
    const auto file = loadHexFixture ("realtime-message.mid.hex");
    REQUIRE_FALSE (file.empty());

    MidiFileReader reader;
    REQUIRE (reader.readData (file.data(), file.size()));
    REQUIRE (reader.tracks().size() == 1);

    const auto& events = reader.tracks()[0];
    REQUIRE (events.size() == 4);
    REQUIRE (events[1].status == 0xf8);
    REQUIRE (events[1].tick == 10);
    REQUIRE (events[1].data1 == 0);
    REQUIRE (events[2].isNoteOff());
    REQUIRE (events[2].tick == 20);
}

TEST_CASE ("MidiFileReader continues reordering after an unmatched same-tick note-on",
           "[midi][import][regression][issue-463]")
{
    const auto file = loadHexFixture ("same-tick-retrigger.mid.hex");
    REQUIRE_FALSE (file.empty());

    MidiFileReader reader;
    REQUIRE (reader.readData (file.data(), file.size()));
    REQUIRE (reader.tracks().size() == 1);

    const auto& events = reader.tracks()[0];
    REQUIRE (events.size() == 7);
    REQUIRE (events[1].tick == 100);
    REQUIRE (events[1].isNoteOn());
    REQUIRE (events[1].noteNumber() == 60);
    REQUIRE (events[2].tick == 100);
    REQUIRE (events[2].isNoteOff());
    REQUIRE (events[2].noteNumber() == 64);
    REQUIRE (events[3].tick == 100);
    REQUIRE (events[3].isNoteOn());
    REQUIRE (events[3].noteNumber() == 64);
}

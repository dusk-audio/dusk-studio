#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_core/juce_core.h>

#include "engine/FileImporter.h"
#include "session/Session.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

namespace
{
struct ByteWriter
{
    std::vector<std::uint8_t> bytes;

    void u8 (int v)            { bytes.push_back ((std::uint8_t) v); }
    void u16 (int v)           { u8 ((v >> 8) & 0xff); u8 (v & 0xff); }
    void u32 (std::uint32_t v) { u8 ((int) (v >> 24)); u8 ((int) (v >> 16));
                                 u8 ((int) (v >> 8)); u8 ((int) v); }
    void tag (const char* s)   { for (int i = 0; i < 4; ++i) u8 (s[i]); }
};

// One-track file at `ppq` ticks per quarter holding a single note and an
// end-of-track marker.
std::vector<std::uint8_t> singleNoteFile (int ppq, int startTick, int lengthTicks, int endTick)
{
    ByteWriter track;
    auto delta = [&track] (int v)
    {
        // Every fixture tick here fits a two-byte variable-length value.
        if (v < 0x80) { track.u8 (v); return; }
        track.u8 (0x80 | ((v >> 7) & 0x7f));
        track.u8 (v & 0x7f);
    };

    delta (startTick);              track.u8 (0x90); track.u8 (60); track.u8 (100);
    delta (lengthTicks);            track.u8 (0x80); track.u8 (60); track.u8 (0);
    delta (endTick - startTick - lengthTicks);
    track.u8 (0xff); track.u8 (0x2f); track.u8 (0x00);

    ByteWriter file;
    file.tag ("MThd");
    file.u32 (6);
    file.u16 (0);
    file.u16 (1);
    file.u16 (ppq);
    file.tag ("MTrk");
    file.u32 ((std::uint32_t) track.bytes.size());
    file.bytes.insert (file.bytes.end(), track.bytes.begin(), track.bytes.end());
    return file.bytes;
}

struct ScopedMidiFile
{
    juce::File file;

    explicit ScopedMidiFile (const std::vector<std::uint8_t>& bytes)
        : file (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("dusk-import-"
                                     + juce::String (juce::Random::getSystemRandom().nextInt())
                                     + ".mid"))
    {
        file.replaceWithData (bytes.data(), bytes.size());
    }

    ~ScopedMidiFile() { file.deleteFile(); }
};
} // namespace

TEST_CASE ("importMidi rescales source ticks to the session resolution", "[import][midi]")
{
    // 96 PPQ: one beat in, one beat long, track ends four beats in.
    const ScopedMidiFile source (singleNoteFile (96, 96, 96, 384));

    fileimport::MidiImportRequest req;
    req.source            = source.file;
    req.sessionSampleRate = 48000.0;
    req.sessionBpm        = 120.0f;
    req.timelineStart     = 1234;

    const auto result = fileimport::importMidi (req);
    REQUIRE (result.ok);
    REQUIRE (result.region.timelineStart == 1234);
    REQUIRE (result.region.notes.size() == 1);

    const auto& note = result.region.notes.front();
    REQUIRE (note.noteNumber == 60);
    REQUIRE (note.velocity == 100);
    REQUIRE (note.channel == 1);
    REQUIRE (note.startTick == kMidiTicksPerQuarter);
    REQUIRE (note.lengthInTicks == kMidiTicksPerQuarter);

    // The end-of-track marker sets the region length, not the last note-off.
    REQUIRE (result.region.lengthInTicks == 4 * kMidiTicksPerQuarter);
    REQUIRE_THAT (result.region.recordedAtBPM, WithinAbs (120.0, 1.0e-9));
}

TEST_CASE ("importMidi closes a note retriggered without a note-off", "[import][midi]")
{
    ByteWriter track;
    auto delta = [&track] (int v) { track.u8 (v); };

    delta (0);   track.u8 (0x90); track.u8 (60); track.u8 (100);
    delta (48);  track.u8 (0x90); track.u8 (60); track.u8 (90);
    delta (48);  track.u8 (0x80); track.u8 (60); track.u8 (0);
    delta (0);   track.u8 (0xff); track.u8 (0x2f); track.u8 (0x00);

    ByteWriter file;
    file.tag ("MThd"); file.u32 (6); file.u16 (0); file.u16 (1); file.u16 (96);
    file.tag ("MTrk"); file.u32 ((std::uint32_t) track.bytes.size());
    file.bytes.insert (file.bytes.end(), track.bytes.begin(), track.bytes.end());

    const ScopedMidiFile source (file.bytes);

    fileimport::MidiImportRequest req;
    req.source            = source.file;
    req.sessionSampleRate = 48000.0;
    req.sessionBpm        = 120.0f;

    const auto result = fileimport::importMidi (req);
    REQUIRE (result.ok);
    REQUIRE (result.region.notes.size() == 2);

    auto notes = result.region.notes;
    std::sort (notes.begin(), notes.end(),
               [] (const auto& a, const auto& b) { return a.startTick < b.startTick; });

    REQUIRE (notes[0].startTick == 0);
    REQUIRE (notes[0].lengthInTicks == kMidiTicksPerQuarter / 2);
    REQUIRE (notes[1].startTick == kMidiTicksPerQuarter / 2);
    REQUIRE (notes[1].lengthInTicks == kMidiTicksPerQuarter / 2);
}

TEST_CASE ("importMidi places an SMPTE-timed file with the session tempo", "[import][midi]")
{
    ByteWriter track;
    track.u8 (0);                                            // note on at t = 0
    track.u8 (0x90); track.u8 (60); track.u8 (100);
    track.u8 (0x83); track.u8 (0x74);                        // +500 ticks = 0.5 s
    track.u8 (0x80); track.u8 (60); track.u8 (0);
    track.u8 (0x00);
    track.u8 (0xff); track.u8 (0x2f); track.u8 (0x00);

    ByteWriter file;
    file.tag ("MThd"); file.u32 (6); file.u16 (0); file.u16 (1);
    file.u16 (0xe728);                                       // 25 fps x 40 subframes
    file.tag ("MTrk"); file.u32 ((std::uint32_t) track.bytes.size());
    file.bytes.insert (file.bytes.end(), track.bytes.begin(), track.bytes.end());

    const ScopedMidiFile source (file.bytes);

    fileimport::MidiImportRequest req;
    req.source            = source.file;
    req.sessionSampleRate = 48000.0;
    req.sessionBpm        = 120.0f;

    const auto result = fileimport::importMidi (req);
    REQUIRE (result.ok);
    REQUIRE (result.region.notes.size() == 1);
    // Half a second at 120 BPM is one beat.
    REQUIRE (result.region.notes.front().lengthInTicks == kMidiTicksPerQuarter);
}

TEST_CASE ("importMidi rejects files it cannot use", "[import][midi]")
{
    fileimport::MidiImportRequest req;
    req.sessionSampleRate = 48000.0;
    req.sessionBpm        = 120.0f;

    req.source = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("dusk-import-does-not-exist.mid");
    REQUIRE_FALSE (fileimport::importMidi (req).ok);

    const ScopedMidiFile garbage (std::vector<std::uint8_t> { 'n', 'o', 'p', 'e', 0, 0, 0, 6 });
    req.source = garbage.file;
    REQUIRE_FALSE (fileimport::importMidi (req).ok);

    const ScopedMidiFile empty (singleNoteFile (96, 0, 0, 0));
    req.source     = empty.file;
    req.sessionBpm = 0.0f;
    REQUIRE_FALSE (fileimport::importMidi (req).ok);
}

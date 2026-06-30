#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "engine/DpImporter.h"

#include <cstring>
#include <memory>
#include <stdexcept>

namespace
{
void writeTestWav (const juce::File& outFile, double sampleRate,
                   int numChannels, int numSamples)
{
    juce::AudioBuffer<float> buf (numChannels, numSamples);
    buf.clear();
    for (int c = 0; c < numChannels; ++c)
    {
        auto* dst = buf.getWritePointer (c);
        for (int n = 0; n < numSamples; ++n)
            dst[n] = 0.25f * (float) ((n % 100) - 50) / 50.0f;
    }

    std::unique_ptr<juce::FileOutputStream> stream (outFile.createOutputStream());
    REQUIRE (stream != nullptr);
    REQUIRE (stream->openedOk());
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (stream.get(), sampleRate,
                             (unsigned int) numChannels, 16, {}, 0));
    REQUIRE (writer != nullptr);
    stream.release();
    REQUIRE (writer->writeFromAudioSampleBuffer (buf, 0, numSamples));
    writer.reset();
}

// Build a minimal valid 2996-byte song.sys: "DP-24   " magic + 24 strip
// records that satisfy the importer's sentinel/format check. fader and pan
// bytes default to calibrated unity 0 dB (0x6A = 106) / centre (0x40). 0x69 is the
// DP-24 power-on default, which the importer maps slightly below 0 dB — not unity.
void writeSongSys (const juce::File& f,
                   juce::uint8 fader0 = 0x6A, juce::uint8 pan0 = 0x40)
{
    juce::MemoryBlock mb;
    mb.setSize (2996, true);
    auto* d = (juce::uint8*) mb.getData();
    std::memcpy (d, "DP-24   ", 8);
    for (int c = 0; c < 24; ++c)
        d[0x14 + c * 20 + 19] = 0x2A;   // strip-array sentinel still gates decode
    // Fader + pan live in a separate array at 0xc4 (stride 20). Default faders
    // to unity (106 = 0 dB) and pan to centre (64); track 0 -> entry 0 carries
    // the test values.
    for (int i = 0; i < 18; ++i) { d[0xc4 + i * 20] = 106; d[0xc4 + i * 20 + 2] = 64; }
    d[0xc4]     = fader0;
    d[0xc4 + 2] = pan0;
    REQUIRE (f.replaceWithData (mb.getData(), mb.getSize()));
}

// Minimal edltable.sys that identifies the device as a DP-24 (model tag at
// 0x08, read by readDeviceModel). "TEAC" magic at 0x00 satisfies the File-List
// scanner; with no ZZ entries the active-fragment set stays empty so the caller
// imports everything. fader/pan recall is gated on a confirmed DP-24, so the
// fader/pan tests must supply this for the array layout to be applied.
void writeEdlTableDp24 (const juce::File& f)
{
    juce::MemoryBlock mb;
    mb.setSize (0x40, true);   // zero-filled, comfortably over the 0x20 minimum
    auto* d = (juce::uint8*) mb.getData();
    std::memcpy (d, "TEAC", 4);
    std::memcpy (d + 0x08, "DP-24", 5);
    REQUIRE (f.replaceWithData (mb.getData(), mb.getSize()));
}

// song.sys with explicit EQ bytes on channel 0 (bytes 4..11 of the strip).
void writeSongSysEq (const juce::File& f, const juce::uint8 eq[8])
{
    juce::MemoryBlock mb;
    mb.setSize (2996, true);
    auto* d = (juce::uint8*) mb.getData();
    std::memcpy (d, "DP-24   ", 8);
    for (int c = 0; c < 24; ++c)
    {
        auto* rec = d + 0x14 + c * 20;
        rec[16] = 0x69; rec[18] = 0x40; rec[19] = 0x2A;
        if (c == 0) for (int k = 0; k < 8; ++k) rec[4 + k] = eq[k];
    }
    REQUIRE (f.replaceWithData (mb.getData(), mb.getSize()));
}

struct TempScope
{
    juce::File dir;
    TempScope()
    {
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-dpimporter-tests")
                  .getChildFile (juce::Uuid().toDashedString());
        const auto r = dir.createDirectory();
        if (r.failed())
            throw std::runtime_error ("TempScope failed: "
                                       + r.getErrorMessage().toStdString());
    }
    ~TempScope() { dir.deleteRecursively(); }
};
} // namespace

TEST_CASE ("DpImporter: pairs stereo, ignores sidecars and master", "[DpImporter]")
{
    using namespace duskstudio::dp;
    TempScope tmp;
    constexpr double kSr = 48000.0;

    writeTestWav (tmp.dir.getChildFile ("ZZ0000_1.wav"), kSr, 1, 4800);
    writeTestWav (tmp.dir.getChildFile ("ZZ0001_1.wav"), kSr, 1, 9600);
    writeTestWav (tmp.dir.getChildFile ("ZZ0001_2.wav"), kSr, 1, 9600);
    writeTestWav (tmp.dir.getChildFile ("MySong.WAV"),   kSr, 2, 4800);  // master
    writeTestWav (tmp.dir.getChildFile ("MySong_z.WAV"), kSr, 2, 4800);  // undo
    writeSongSys (tmp.dir.getChildFile ("song.sys"));
    // AppleDouble sidecar - must be ignored.
    tmp.dir.getChildFile ("._ZZ0000_1.wav").replaceWithText ("junk");

    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.ok);
    REQUIRE (scan.tracks.size() == 2);
    REQUIRE (std::abs (scan.sampleRate - kSr) < 0.5);   // float: tolerance, not ==
    REQUIRE (scan.bitDepth == 16);                       // int: == is correct

    // Track 1 = ZZ0000 mono; track 2 = ZZ0001 stereo pair.
    REQUIRE_FALSE (scan.tracks[0].fragment.stereo);
    REQUIRE (scan.tracks[0].fragment.lengthSamples == 4800);
    REQUIRE (scan.tracks[1].fragment.stereo);
    REQUIRE (scan.tracks[1].fragment.lengthSamples == 9600);
    REQUIRE (scan.stereoPairs == 1);
    REQUIRE (scan.tracks[0].timelineStart == 0);
}

TEST_CASE ("DpImporter: gappy indices ordered ascending", "[DpImporter]")
{
    using namespace duskstudio::dp;
    TempScope tmp;
    constexpr double kSr = 44100.0;
    writeTestWav (tmp.dir.getChildFile ("ZZ0005_1.wav"), kSr, 1, 1000);
    writeTestWav (tmp.dir.getChildFile ("ZZ0000_1.wav"), kSr, 1, 1000);
    writeTestWav (tmp.dir.getChildFile ("ZZ0002_1.wav"), kSr, 1, 1000);

    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.tracks.size() == 3);
    REQUIRE (scan.tracks[0].fragment.zzIndex == 0);
    REQUIRE (scan.tracks[1].fragment.zzIndex == 2);
    REQUIRE (scan.tracks[2].fragment.zzIndex == 5);
    REQUIRE (scan.tracks[0].name == "DP 0000");
}

TEST_CASE ("DpImporter: lone _2 imported as mono with warning", "[DpImporter]")
{
    using namespace duskstudio::dp;
    TempScope tmp;
    writeTestWav (tmp.dir.getChildFile ("ZZ0003_2.wav"), 48000.0, 1, 2000);

    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.tracks.size() == 1);
    REQUIRE_FALSE (scan.tracks[0].fragment.stereo);
    REQUIRE (scan.tracks[0].fragment.lengthSamples == 2000);
    REQUIRE (scan.warnings.containsIgnoreCase ("right channel without left"));
}

TEST_CASE ("DpImporter: empty folder and garbage never throw", "[DpImporter]")
{
    using namespace duskstudio::dp;
    TempScope tmp;

    SECTION ("no fragments")
    {
        const auto scan = scanSongFolder (tmp.dir);
        REQUIRE_FALSE (scan.ok);
        REQUIRE (scan.tracks.empty());
    }
    SECTION ("garbage side-files + truncated wav")
    {
        tmp.dir.getChildFile ("edltable.sys").replaceWithText ("not a real edl");
        tmp.dir.getChildFile ("song.sys").replaceWithText ("too short");
        tmp.dir.getChildFile ("ZZ0000_1.wav").replaceWithText ("RIFFxxxxx");
        const auto scan = scanSongFolder (tmp.dir);
        // Bad WAV header skipped -> no importable track, no crash.
        REQUIRE_FALSE (scan.ok);
        REQUIRE_FALSE (scan.mixerDecoded);
        REQUIRE_FALSE (scan.timelineDecoded);
    }
    SECTION ("nonexistent folder")
    {
        const auto scan = scanSongFolder (tmp.dir.getChildFile ("does-not-exist"));
        REQUIRE_FALSE (scan.ok);
    }
}

TEST_CASE ("DpImporter: decodes mixer fader/pan from song.sys", "[DpImporter]")
{
    using namespace duskstudio::dp;
    TempScope tmp;
    writeTestWav (tmp.dir.getChildFile ("ZZ0000_1.wav"), 48000.0, 1, 1000);
    // ch0 fader = max (0x7F => +6 dB), pan = hard right (0x7F => +1).
    writeSongSys (tmp.dir.getChildFile ("song.sys"), 0x7F, 0x7F);
    // Fader/pan recall requires a confirmed DP-24 model.
    writeEdlTableDp24 (tmp.dir.getChildFile ("edltable.sys"));

    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.ok);
    REQUIRE (scan.mixerDecoded);
    REQUIRE (scan.tracks[0].mixer.valid);
    REQUIRE (scan.tracks[0].mixer.faderDb > 5.5f);
    REQUIRE (scan.tracks[0].mixer.pan > 0.9f);
}

TEST_CASE ("DpImporter: decodes calibrated fader levels (SONG_0002)", "[DpImporter]")
{
    using namespace duskstudio::dp;
    using Catch::Matchers::WithinAbs;
    TempScope tmp;

    // 24 mono fragments -> scan.tracks[0..23] map to tracks 1..24 in ZZ order.
    for (int i = 0; i < 24; ++i)
        writeTestWav (tmp.dir.getChildFile (juce::String::formatted ("ZZ%04d_1.wav", i)),
                      48000.0, 1, 1000);

    // The 18 real fader bytes from SONG_0002's 0xc4 array (entries 0..17).
    const juce::uint8 faders[18] =
        { 0, 3, 16, 26, 40, 55, 65, 74, 85, 94, 100, 106, 107, 112, 118, 120, 127, 42 };
    {
        juce::MemoryBlock mb; mb.setSize (2996, true);
        auto* d = (juce::uint8*) mb.getData();
        std::memcpy (d, "DP-24   ", 8);
        for (int c = 0; c < 24; ++c) { auto* r = d + 0x14 + c * 20; r[18] = 0x40; r[19] = 0x2A; }
        for (int i = 0; i < 18; ++i) d[0xc4 + i * 20] = faders[i];
        tmp.dir.getChildFile ("song.sys").replaceWithData (mb.getData(), mb.getSize());
    }
    // Fader recall requires a confirmed DP-24 model.
    writeEdlTableDp24 (tmp.dir.getChildFile ("edltable.sys"));

    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.ok);
    REQUIRE (scan.tracks.size() == 24);
    auto fdb = [&] (int track) { return scan.tracks[(size_t) track].mixer.faderDb; };

    REQUIRE (fdb (0) <= -99.0f);                        // track 1, byte 0 -> -inf
    REQUIRE_THAT (fdb (4),  WithinAbs (-20.0f, 0.01f)); // track 5,  byte 40
    REQUIRE_THAT (fdb (11), WithinAbs (  0.0f, 0.01f)); // track 12, byte 106 -> unity
    REQUIRE_THAT (fdb (20), WithinAbs (  6.0f, 0.01f)); // track 21, byte 127 -> max
    REQUIRE_THAT (fdb (22), WithinAbs (-19.1f, 0.01f)); // track 23, byte 42

    // Tracks 13-24 are stereo pairs sharing one fader entry.
    REQUIRE (fdb (12) == fdb (13));                     // 13 & 14 -> entry 12
    REQUIRE_THAT (fdb (12), WithinAbs (0.3f, 0.01f));   // byte 107 -> +0.3
}

TEST_CASE ("DpImporter: decodes calibrated pan (SONG_0002)", "[DpImporter]")
{
    using namespace duskstudio::dp;
    using Catch::Matchers::WithinAbs;
    TempScope tmp;
    for (int i = 0; i < 4; ++i)
        writeTestWav (tmp.dir.getChildFile (juce::String::formatted ("ZZ%04d_1.wav", i)),
                      48000.0, 1, 1000);

    // Real pan bytes from SONG_0002 (0xc4 record +2): L63->1, R63->127, L14->50, R25->89.
    const juce::uint8 pans[4] = { 1, 127, 50, 89 };
    {
        juce::MemoryBlock mb; mb.setSize (2996, true);
        auto* d = (juce::uint8*) mb.getData();
        std::memcpy (d, "DP-24   ", 8);
        for (int c = 0; c < 24; ++c) d[0x14 + c * 20 + 19] = 0x2A;
        for (int i = 0; i < 18; ++i) { d[0xc4 + i * 20] = 106; d[0xc4 + i * 20 + 2] = 64; }
        for (int i = 0; i < 4; ++i) d[0xc4 + i * 20 + 2] = pans[i];
        tmp.dir.getChildFile ("song.sys").replaceWithData (mb.getData(), mb.getSize());
    }
    // Pan recall requires a confirmed DP-24 model.
    writeEdlTableDp24 (tmp.dir.getChildFile ("edltable.sys"));

    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.ok);
    auto pan = [&] (int t) { return scan.tracks[(size_t) t].mixer.pan; };
    REQUIRE_THAT (pan (0), WithinAbs (-1.0f, 0.001f));             // L63 full left
    REQUIRE_THAT (pan (1), WithinAbs ( 1.0f, 0.001f));             // R63 full right
    REQUIRE_THAT (pan (2), WithinAbs (-14.0f / 63.0f, 0.01f));     // L14
    REQUIRE_THAT (pan (3), WithinAbs ( 25.0f / 63.0f, 0.01f));     // R25
}

TEST_CASE ("DpImporter: decodes 3-band EQ from song.sys", "[DpImporter]")
{
    using namespace duskstudio::dp;
    TempScope tmp;
    writeTestWav (tmp.dir.getChildFile ("ZZ0000_1.wav"), 48000.0, 1, 1000);
    // EQ bytes 4..11 in HIGH,MID,LOW order (verified on SONG_0005):
    // on; HighGain=6(-6dB)/Freq idx20(7.5k); MidGain=8(-4dB)/Freq idx25(1k)/Q idx3(2.0);
    // LowGain=18(+6dB)/Freq idx4(70Hz).
    const juce::uint8 eq[8] = { 1, 6, 20, 8, 25, 3, 18, 4 };
    writeSongSysEq (tmp.dir.getChildFile ("song.sys"), eq);

    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.ok);
    REQUIRE (scan.mixerDecoded);
    const auto& m = scan.tracks[0].mixer;
    REQUIRE (m.eqOn);
    REQUIRE (std::abs (m.highGainDb - (-6.0f)) < 0.01f);
    REQUIRE (std::abs (m.highFreqHz - 7500.0f) < 0.01f);
    REQUIRE (std::abs (m.midGainDb  - (-4.0f)) < 0.01f);
    REQUIRE (std::abs (m.midFreqHz  - 1000.0f) < 0.01f);
    REQUIRE (std::abs (m.midQ       -    2.0f) < 0.01f);
    REQUIRE (std::abs (m.lowGainDb  -    6.0f) < 0.01f);
    REQUIRE (std::abs (m.lowFreqHz  -   70.0f) < 0.01f);
}

// song.sys carrying a clip start/end pair (samples) + song length, so the
// position decoder can recover a non-zero start by length-matching.
void writeSongSysClip (const juce::File& f, juce::uint32 songLen,
                       juce::uint32 startSamples, juce::uint32 endSamples)
{
    juce::MemoryBlock mb;
    mb.setSize (2996, true);
    auto* d = (juce::uint8*) mb.getData();
    std::memcpy (d, "DP-24   ", 8);
    for (int c = 0; c < 24; ++c)
    {
        auto* rec = d + 0x14 + c * 20;
        rec[16] = 0x69; rec[18] = 0x40; rec[19] = 0x2A;
    }
    auto put = [&] (int off, juce::uint32 v) { std::memcpy (d + off, &v, 4); };   // LE host
    put (0x364, songLen);       // song length
    put (0x388, startSamples);  // clip start
    put (0x390, endSamples);    // clip end (= start + length - 480)
    REQUIRE (f.replaceWithData (mb.getData(), mb.getSize()));
}

// song.sys with locate-mark records: {u32 pos, u32 index<<24} at 0x388 stride 8.
void writeSongSysMarkers (const juce::File& f, juce::uint32 songLen,
                          const std::vector<std::pair<juce::uint32,int>>& marks)
{
    juce::MemoryBlock mb;
    mb.setSize (2996, true);
    auto* d = (juce::uint8*) mb.getData();
    std::memcpy (d, "DP-24   ", 8);
    for (int c = 0; c < 24; ++c) { auto* r = d + 0x14 + c*20; r[16]=0x69; r[18]=0x40; r[19]=0x2A; }
    auto put = [&] (int off, juce::uint32 v) { std::memcpy (d + off, &v, 4); };
    put (0x364, songLen);
    int o = 0x388;
    for (auto& [pos, idx] : marks) { put (o, pos); put (o + 4, (juce::uint32) idx << 24); o += 8; }
    REQUIRE (f.replaceWithData (mb.getData(), mb.getSize()));
}

TEST_CASE ("DpImporter: decodes song tempo from song.sys", "[DpImporter]")
{
    using namespace duskstudio::dp;
    TempScope tmp;
    writeTestWav (tmp.dir.getChildFile ("ZZ0000_1.wav"), 48000.0, 1, 1000);
    {
        juce::MemoryBlock mb; mb.setSize (2996, true);
        auto* d = (juce::uint8*) mb.getData();
        std::memcpy (d, "DP-24   ", 8);
        for (int c = 0; c < 24; ++c) { auto* r = d + 0x14 + c*20; r[16]=0x69; r[18]=0x40; r[19]=0x2A; }
        d[0x6d8] = 107;   // tempo BPM
        tmp.dir.getChildFile ("song.sys").replaceWithData (mb.getData(), mb.getSize());
    }
    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.ok);
    REQUIRE (scan.tempoBpm == 107);
}

TEST_CASE ("DpImporter: decodes time signature from song.sys", "[DpImporter]")
{
    using namespace duskstudio::dp;

    // byte @0x6d9 = 12*log2(denom) + (numerator-1). Values lifted from songs
    // authored on a real DP-24 (SONG_0002..0005).
    auto scanWith = [] (juce::uint8 packed)
    {
        TempScope tmp;
        writeTestWav (tmp.dir.getChildFile ("ZZ0000_1.wav"), 48000.0, 1, 1000);
        juce::MemoryBlock mb; mb.setSize (2996, true);
        auto* d = (juce::uint8*) mb.getData();
        std::memcpy (d, "DP-24   ", 8);
        for (int c = 0; c < 24; ++c) { auto* r = d + 0x14 + c*20; r[16]=0x69; r[18]=0x40; r[19]=0x2A; }
        d[0x6d9] = packed;
        tmp.dir.getChildFile ("song.sys").replaceWithData (mb.getData(), mb.getSize());
        return scanSongFolder (tmp.dir);
    };

    SECTION ("1/1")  { const auto s = scanWith (0);  REQUIRE (s.timeSigNum == 1);  REQUIRE (s.timeSigDen == 1); }
    SECTION ("3/2")  { const auto s = scanWith (14); REQUIRE (s.timeSigNum == 3);  REQUIRE (s.timeSigDen == 2); }
    SECTION ("8/4")  { const auto s = scanWith (31); REQUIRE (s.timeSigNum == 8);  REQUIRE (s.timeSigDen == 4); }
    SECTION ("12/8") { const auto s = scanWith (47); REQUIRE (s.timeSigNum == 12); REQUIRE (s.timeSigDen == 8); }
    SECTION ("4/4 default") { const auto s = scanWith (27); REQUIRE (s.timeSigNum == 4); REQUIRE (s.timeSigDen == 4); }
    SECTION ("out-of-range byte rejected")
    {
        const auto s = scanWith (48);   // denExp 4 → /16, which DP units don't have
        REQUIRE (s.timeSigNum == 0);
        REQUIRE (s.timeSigDen == 0);
    }
}

TEST_CASE ("DpImporter: decodes song markers", "[DpImporter]")
{
    using namespace duskstudio::dp;
    TempScope tmp;
    writeTestWav (tmp.dir.getChildFile ("ZZ0000_1.wav"), 48000.0, 1, 1000);
    writeSongSysMarkers (tmp.dir.getChildFile ("song.sys"), 10000,
                         { { 4800, 1 }, { 9600, 2 } });

    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.ok);
    REQUIRE (scan.markers.size() == 2);
    REQUIRE (scan.markers[0].positionSamples == 4800);
    REQUIRE (scan.markers[0].index == 1);
    REQUIRE (scan.markers[1].positionSamples == 9600);
    REQUIRE (scan.markers[1].index == 2);
}

TEST_CASE ("DpImporter: recovers clip start from song.sys (no mixdown)", "[DpImporter]")
{
    using namespace duskstudio::dp;
    TempScope tmp;
    // ZZ0000 (len 1000) has no stored end -> stays 0; ZZ0001 (len 500) placed at
    // 5000 with end = start + length - 480 = 5020 (device's column-aligned end).
    writeTestWav (tmp.dir.getChildFile ("ZZ0000_1.wav"), 48000.0, 1, 1000);
    writeTestWav (tmp.dir.getChildFile ("ZZ0001_1.wav"), 48000.0, 1, 500);
    writeSongSysClip (tmp.dir.getChildFile ("song.sys"), 10000, 5000, 5020);

    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.ok);
    REQUIRE (scan.tracks.size() == 2);
    REQUIRE (scan.timelineDecoded);
    REQUIRE (scan.tracks[0].timelineStart == 0);      // no matching end -> song start
    REQUIRE (scan.tracks[1].timelineStart == 5000);   // length-matched
}

TEST_CASE ("DpImporter: File-List filters out discarded takes", "[DpImporter]")
{
    using namespace duskstudio::dp;
    TempScope tmp;
    // Three fragments on disk; edltable File-List references only two -> the
    // third (ZZ0002) is a discarded take and must be skipped.
    writeTestWav (tmp.dir.getChildFile ("ZZ0000_1.wav"), 48000.0, 1, 1000);
    writeTestWav (tmp.dir.getChildFile ("ZZ0001_1.wav"), 48000.0, 1, 1000);
    writeTestWav (tmp.dir.getChildFile ("ZZ0002_1.wav"), 48000.0, 1, 1000);   // discarded take
    {
        // Minimal edltable with the "TEAC" signature, whose only ZZ filename
        // strings are 0000 + 0001.
        juce::MemoryBlock mb; mb.setSize (512, true);
        auto* d = (char*) mb.getData();
        std::memcpy (d, "TEAC", 4);
        std::memcpy (d + 0x40, "ZZ0000_1.wav", 12);
        std::memcpy (d + 0x60, "ZZ0001_1.wav", 12);
        tmp.dir.getChildFile ("edltable.sys").replaceWithData (mb.getData(), mb.getSize());
    }
    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.ok);
    REQUIRE (scan.tracks.size() == 2);
    REQUIRE (scan.discardedTakes == 1);
    for (const auto& t : scan.tracks) REQUIRE (t.fragment.zzIndex != 2);
}

TEST_CASE ("DpImporter: looksLikeSongFolder heuristic", "[DpImporter]")
{
    using namespace duskstudio::dp;
    TempScope tmp;
    REQUIRE_FALSE (looksLikeSongFolder (tmp.dir));
    writeTestWav (tmp.dir.getChildFile ("ZZ0000_1.wav"), 48000.0, 1, 100);
    REQUIRE (looksLikeSongFolder (tmp.dir));
}

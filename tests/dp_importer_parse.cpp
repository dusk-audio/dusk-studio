#include <catch2/catch_test_macros.hpp>

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
// bytes default to unity (0x69) / centre (0x40).
void writeSongSys (const juce::File& f,
                   juce::uint8 fader0 = 0x69, juce::uint8 pan0 = 0x40)
{
    juce::MemoryBlock mb;
    mb.setSize (2996, true);
    auto* d = (juce::uint8*) mb.getData();
    std::memcpy (d, "DP-24   ", 8);
    for (int c = 0; c < 24; ++c)
    {
        auto* rec = d + 0x14 + c * 20;
        rec[16] = (c == 0) ? fader0 : 0x69;
        rec[18] = (c == 0) ? pan0   : 0x40;
        rec[19] = 0x2A;   // sentinel
    }
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
    REQUIRE (scan.sampleRate == kSr);
    REQUIRE (scan.bitDepth == 16);

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

    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.ok);
    REQUIRE (scan.mixerDecoded);
    REQUIRE (scan.tracks[0].mixer.valid);
    REQUIRE (scan.tracks[0].mixer.faderDb > 5.5f);
    REQUIRE (scan.tracks[0].mixer.pan > 0.9f);
}

TEST_CASE ("DpImporter: decodes 3-band EQ from song.sys", "[DpImporter]")
{
    using namespace duskstudio::dp;
    TempScope tmp;
    writeTestWav (tmp.dir.getChildFile ("ZZ0000_1.wav"), 48000.0, 1, 1000);
    // EQ bytes 4..11: on, LowGain=18(+6dB)/Freq idx0(32Hz),
    // MidGain=6(-6dB)/Freq idx63(18k)/Q idx4(4.0), HighGain=24(+12dB)/Freq idx31(18k).
    const juce::uint8 eq[8] = { 1, 18, 0, 6, 63, 4, 24, 31 };
    writeSongSysEq (tmp.dir.getChildFile ("song.sys"), eq);

    const auto scan = scanSongFolder (tmp.dir);
    REQUIRE (scan.ok);
    REQUIRE (scan.mixerDecoded);
    const auto& m = scan.tracks[0].mixer;
    REQUIRE (m.eqOn);
    REQUIRE (std::abs (m.lowGainDb  -   6.0f) < 0.01f);
    REQUIRE (std::abs (m.lowFreqHz  -  32.0f) < 0.01f);
    REQUIRE (std::abs (m.midGainDb  - (-6.0f)) < 0.01f);
    REQUIRE (std::abs (m.midFreqHz  - 18000.0f) < 0.01f);
    REQUIRE (std::abs (m.midQ       -   4.0f) < 0.01f);
    REQUIRE (std::abs (m.highGainDb -  12.0f) < 0.01f);
    REQUIRE (std::abs (m.highFreqHz - 18000.0f) < 0.01f);
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
        // Minimal edltable whose only ZZ filename strings are 0000 + 0001.
        juce::MemoryBlock mb; mb.setSize (512, true);
        auto* d = (char*) mb.getData();
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

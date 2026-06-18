#include <catch2/catch_test_macros.hpp>

#include "../src/engine/LameMp3Writer.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>

using namespace duskstudio;

namespace
{
// Encode `src` to a fresh temp .mp3 via LameMp3Writer and return the file.
juce::File encodeToMp3 (const juce::AudioBuffer<float>& src, double sr, int bitrate)
{
    const auto mp3 = juce::File::createTempFile (".mp3");
    auto* stream = mp3.createOutputStream().release();
    LameMp3Writer writer (stream, sr, 2, bitrate);
    REQUIRE (writer.isOk());
    REQUIRE (writer.writeFromAudioSampleBuffer (src, 0, src.getNumSamples()));
    return mp3;   // writer destructor (end of scope) flushes + closes
}
}

// JUCE bundles no MP3 *decoder* by default, so verify the bytes structurally:
// a valid MPEG-1 Layer III frame header at the chosen rate/bitrate, a file size
// consistent with CBR, and — the part that proves the audio actually reached the
// encoder rather than being dropped — a sine encoding that differs from silence.
TEST_CASE ("LameMp3Writer emits a valid 48k/320k MP3 carrying the signal", "[mp3]")
{
    constexpr double sr      = 48000.0;
    constexpr int    n       = (int) sr;          // 1 second
    constexpr int    bitrate = 320;
    constexpr float  freq    = 1000.0f;

    juce::AudioBuffer<float> sine (2, n), silence (2, n);
    silence.clear();
    for (int i = 0; i < n; ++i)
    {
        const float s = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                              * freq * (float) i / (float) sr);
        sine.setSample (0, i, s);
        sine.setSample (1, i, s);
    }

    const auto sineMp3    = encodeToMp3 (sine,    sr, bitrate);
    const auto silenceMp3 = encodeToMp3 (silence, sr, bitrate);

    // ── Size: CBR 320 kbps over 1 s ≈ 40 KB. Allow framing slack. ──
    const auto expectedBytes = (juce::int64) (bitrate * 1000.0 / 8.0);   // 40000
    CHECK (sineMp3.getSize() > expectedBytes * 3 / 4);
    CHECK (sineMp3.getSize() < expectedBytes * 3 / 2);

    // ── Frame header: first frame must be MPEG-1 Layer III, 320 kbps, 48 kHz. ──
    {
        juce::FileInputStream in (sineMp3);
        REQUIRE (in.openedOk());
        juce::uint8 h[4] {};
        REQUIRE (in.read (h, 4) == 4);
        REQUIRE (h[0] == 0xFF);                 // frame sync
        REQUIRE ((h[1] & 0xE0) == 0xE0);        // sync (top 3 bits of byte 1)
        CHECK (((h[1] >> 3) & 0x3) == 3);       // MPEG version 1
        CHECK (((h[1] >> 1) & 0x3) == 1);       // Layer III
        CHECK (((h[2] >> 4) & 0xF) == 14);      // bitrate index 14 = 320 kbps (MPEG1 L3)
        CHECK (((h[2] >> 2) & 0x3) == 1);       // samplerate index 1 = 48 kHz (MPEG1)
    }

    // ── Signal reached the encoder: the sine encoding differs from silence. ──
    juce::MemoryBlock a, b;
    REQUIRE (sineMp3.loadFileAsData (a));
    REQUIRE (silenceMp3.loadFileAsData (b));
    CHECK (a != b);

    sineMp3.deleteFile();
    silenceMp3.deleteFile();
}

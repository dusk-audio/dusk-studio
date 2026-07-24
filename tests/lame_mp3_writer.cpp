#include <catch2/catch_test_macros.hpp>

#include "../src/engine/LameMp3Writer.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <vector>

using namespace duskstudio;

namespace
{
// Encode `src` to a fresh temp .mp3 via LameMp3Writer and return the file.
juce::File encodeToMp3 (const juce::AudioBuffer<float>& src, double sr, int bitrate)
{
    const auto mp3 = juce::File::createTempFile (".mp3");
    LameMp3Writer writer (mp3.getFullPathName().toStdString(), sr, 2, bitrate);
    REQUIRE (writer.isOk());
    const int n = src.getNumSamples();
    std::vector<float> interleaved ((size_t) n * 2, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        interleaved[(size_t) i * 2]     = src.getSample (0, i);
        interleaved[(size_t) i * 2 + 1] = src.getSample (1, i);
    }
    REQUIRE (writer.writeInterleaved (interleaved.data(), n));
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
        juce::MemoryBlock raw;
        REQUIRE (sineMp3.loadFileAsData (raw));
        const auto* d  = (const juce::uint8*) raw.getData();
        const int    sz = (int) raw.getSize();
        // Scan for the first MPEG frame sync (0xFF then 111x in the next byte) -
        // a valid MP3 can carry an ID3/Xing header before frame 1, so the sync
        // isn't guaranteed at offset 0. (Our writer emits no such header, so this
        // finds offset 0 today; the scan keeps the test correct if that changes.)
        // Reject false syncs: 0xFF + 111x also appears inside frame data, so
        // require non-reserved version / layer / bitrate / sample-rate fields so
        // only a genuine frame header matches. The specific 320k/48k values are
        // asserted below, so this doesn't make those CHECKs tautological.
        auto looksLikeFrameHeader = [] (const juce::uint8* h)
        {
            if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
            const int version  = (h[1] >> 3) & 0x3;   // 1 = reserved
            const int layer    = (h[1] >> 1) & 0x3;   // 0 = reserved
            const int bitrate  = (h[2] >> 4) & 0xF;   // 0 = free, 15 = reserved
            const int samprate = (h[2] >> 2) & 0x3;   // 3 = reserved
            return version != 1 && layer != 0 && bitrate != 0 && bitrate != 15 && samprate != 3;
        };
        int off = -1;
        for (int i = 0; i + 4 <= sz; ++i)
            if (looksLikeFrameHeader (d + i)) { off = i; break; }
        REQUIRE (off >= 0);                     // a valid frame header exists
        const juce::uint8* h = d + off;
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

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/PlaybackEngine.h"
#include "engine/Transport.h"
#include "session/Session.h"

#include <juce_audio_formats/juce_audio_formats.h>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

// A block that crosses the loop end must play source material up to loopEnd,
// then continue from loopStart — no bleed past the loop point, no skipped
// downbeat — and the wrapped span must produce real samples even though the
// forward-only BufferingAudioReader is cold at that position (served from the
// loop-start pre-cache primed in preparePlayback).
TEST_CASE ("loop-aware readForTrack wraps at the loop boundary",
           "[playback][loop]")
{
    constexpr double kSampleRate = 48000.0;
    constexpr int    kFileLen    = 48000;

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-loop-read-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

    // Ramp WAV: sample n = n * 1e-5, mono 24-bit.
    const auto wav = dir.getChildFile ("audio/ramp.wav");
    wav.getParentDirectory().createDirectory();
    {
        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            fmt.createWriterFor (wav.createOutputStream().release(),
                                  kSampleRate, 1, 24, {}, 0));
        REQUIRE (writer != nullptr);
        juce::AudioBuffer<float> buf (1, kFileLen);
        for (int n = 0; n < kFileLen; ++n)
            buf.setSample (0, n, (float) n * 1e-5f);
        REQUIRE (writer->writeFromAudioSampleBuffer (buf, 0, kFileLen));
    }

    Session session;
    session.setSessionDirectory (dir);
    {
        AudioRegion r;
        r.file            = wav;
        r.timelineStart   = 0;
        r.lengthInSamples = kFileLen;
        session.track (0).regions.push_back (r);
    }

    Transport transport;
    transport.setLoopRange (1000, 9000);
    transport.setLoopEnabled (true);

    PlaybackEngine pe (session);
    pe.bindTransport (transport);
    pe.prepare (512);
    pe.preparePlayback();

    // Block of 400 at playhead 8800: spans [8800, 9000) then wraps to
    // [1000, 1200). Seam lands at output offset 200.
    std::vector<float> out (400, -1.0f);
    pe.readForTrack (0, 8800, out.data(), nullptr, 400, 1000, 9000);

    // Pre-seam, outside the 64-sample fade-out window (which covers
    // offsets 136..199): raw pre-wrap source values.
    REQUIRE_THAT (out[0],   WithinAbs (8800.0f * 1e-5f, 2e-4f));
    REQUIRE_THAT (out[100], WithinAbs (8900.0f * 1e-5f, 2e-4f));

    // Post-seam, outside the fade-in window (offsets 200..263): wrapped
    // source values from loopStart — NOT the pre-wrap continuation.
    REQUIRE_THAT (out[300], WithinAbs (1100.0f * 1e-5f, 2e-4f));
    REQUIRE_THAT (out[399], WithinAbs (1199.0f * 1e-5f, 2e-4f));

    // Seam declick: the first wrapped sample is fully faded in by +64.
    REQUIRE (std::abs (out[200]) < 1e-4f);

    // Fade-out direction: attenuation deepens INTO the seam. The sample
    // adjacent to the wrap (offset 199) is silenced, the far edge of the
    // 64-sample window (offset 136) keeps full level, and every step in
    // between attenuates monotonically. An inverted ramp leaves the seam
    // discontinuity intact and swells backwards instead. Monotonicity holds
    // for the product of ramp source and gain: the source rises ~0.01% per
    // sample while the raised-cosine falls faster everywhere in the window.
    REQUIRE (std::abs (out[199]) < 1e-3f);
    REQUIRE_THAT (out[136], WithinAbs (8936.0f * 1e-5f, 2e-3f));
    for (int idx = 136; idx < 199; ++idx)
        REQUIRE (std::abs (out[idx + 1]) < std::abs (out[idx]));

    pe.stopPlayback();
}

namespace
{
// numCh-channel WAV where channel c holds the constant level[c]. Constants (not
// a ramp) so a channel swap or a silent channel is unambiguous in the assert.
juce::File writeConstantWav (const juce::File& dir, const juce::String& name,
                             const std::vector<float>& level, int numFrames)
{
    const auto wav = dir.getChildFile ("audio").getChildFile (name);
    wav.getParentDirectory().createDirectory();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        fmt.createWriterFor (wav.createOutputStream().release(),
                              48000.0, (unsigned) level.size(), 24, {}, 0));
    REQUIRE (writer != nullptr);
    juce::AudioBuffer<float> buf ((int) level.size(), numFrames);
    for (size_t c = 0; c < level.size(); ++c)
        for (int n = 0; n < numFrames; ++n)
            buf.setSample ((int) c, n, level[c]);
    REQUIRE (writer->writeFromAudioSampleBuffer (buf, 0, numFrames));
    return wav;
}
} // namespace

// The channel count that decides whether the right channel is read comes from
// the decoded file, not AudioRegion::numChannels. The model's copy can disagree
// with what is on disk (hand-edited session, replaced take), and believing it
// either truncates a stereo take to its left channel or asks a mono file for a
// right one it does not have.
//
// Read through the loop-start pre-cache: primeLoopCaches fills it synchronously
// on the message thread, whereas the reader's own window is warmed by a
// background worker that a read issued immediately after preparePlayback would
// race (returning silence, not a channel bug). The cache fill consults the same
// per-region channel count, so it pins the same decision deterministically.
TEST_CASE ("readForTrack takes the channel count from the file, not the region",
           "[playback][channels]")
{
    constexpr int kFrames = 4096;
    constexpr int kBlock  = 256;

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-chan-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

    SECTION ("mono file, region claims stereo - duplicated to both outputs")
    {
        const auto wav = writeConstantWav (dir, "mono.wav", { 0.5f }, kFrames);

        Session session;
        session.setSessionDirectory (dir);
        AudioRegion r;
        r.file            = wav;
        r.timelineStart   = 0;
        r.lengthInSamples = kFrames;
        r.numChannels     = 2;   // wrong on purpose
        session.track (0).regions.push_back (r);

        Transport transport;
        transport.setLoopRange (0, kFrames);
        transport.setLoopEnabled (true);
        PlaybackEngine pe (session);
        pe.bindTransport (transport);
        pe.prepare (kBlock);
        pe.preparePlayback();

        std::vector<float> outL ((size_t) kBlock, -1.0f), outR ((size_t) kBlock, -1.0f);
        pe.readForTrack (0, 0, outL.data(), outR.data(), kBlock, 0, kFrames);

        // A mono source must land on BOTH outputs, not leave the right silent.
        REQUIRE_THAT (outL[kBlock / 2], WithinAbs (0.5f, 2e-4f));
        REQUIRE_THAT (outR[kBlock / 2], WithinAbs (0.5f, 2e-4f));
        pe.stopPlayback();
    }

    SECTION ("stereo file, region claims mono - channels stay independent")
    {
        const auto wav = writeConstantWav (dir, "stereo.wav", { 0.5f, -0.25f }, kFrames);

        Session session;
        session.setSessionDirectory (dir);
        AudioRegion r;
        r.file            = wav;
        r.timelineStart   = 0;
        r.lengthInSamples = kFrames;
        r.numChannels     = 1;   // wrong on purpose
        session.track (0).regions.push_back (r);

        Transport transport;
        transport.setLoopRange (0, kFrames);
        transport.setLoopEnabled (true);
        PlaybackEngine pe (session);
        pe.bindTransport (transport);
        pe.prepare (kBlock);
        pe.preparePlayback();

        std::vector<float> outL ((size_t) kBlock, 0.0f), outR ((size_t) kBlock, 0.0f);
        pe.readForTrack (0, 0, outL.data(), outR.data(), kBlock, 0, kFrames);

        // Reading by the region's claim would have duplicated L over R.
        REQUIRE_THAT (outL[kBlock / 2], WithinAbs ( 0.5f,  2e-4f));
        REQUIRE_THAT (outR[kBlock / 2], WithinAbs (-0.25f, 2e-4f));
        pe.stopPlayback();
    }
}

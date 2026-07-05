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

    pe.stopPlayback();
}

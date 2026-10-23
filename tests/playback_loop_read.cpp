#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/PlaybackEngine.h"
#include "engine/Transport.h"
#include "session/Session.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <limits>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

// The head must survive independently of a tail-resident rolling window,
// including when the loop is enabled or moved after playback starts.
TEST_CASE ("loop-aware readForTrack wraps at the loop boundary",
           "[playback][loop]")
{
    constexpr double kSampleRate = 48000.0;
    constexpr int    kFileLen    = 240000;
    constexpr std::int64_t kLoopStart = 1000, kLoopEnd = 180000;
    constexpr float kSlope = 1.0e-6f;
    bool initiallyEnabled = true, keepCaptureRange = false, checkSeekReadiness = false, waitOut = false;
    int trimmedFrames = 0, preparedBlock = 512;
    int loopProbe = 0;
    std::int64_t initialStart = kLoopStart;
    SECTION ("Loop enabled before playback") {}
    SECTION ("Loop enabled while rolling") { initiallyEnabled = false; }
    SECTION ("Loop head moved while rolling") { initialStart = 50000; }
    SECTION ("Capture keeps its effective range") { keepCaptureRange = true; }
    SECTION ("A seek waits for an enabled loop's head cache")
    {
        initiallyEnabled = false;
        checkSeekReadiness = true;
    }
    SECTION ("A trimmed 8191-frame region can become seek ready") { trimmedFrames = 8191; }
    SECTION ("A trimmed 8192-frame region can become seek ready") { trimmedFrames = 8192; }
    SECTION ("A trimmed 8193-frame region can become seek ready") { trimmedFrames = 8193; }
    SECTION ("A large callback can become seek ready") { trimmedFrames = preparedBlock = 16384; }
    SECTION ("A cold seek retains the current timeline") { trimmedFrames = kFileLen; }
    SECTION ("A seek whose readers never warm commits once its wait runs out")
    {
        trimmedFrames = kFileLen;
        waitOut = true;
    }
    SECTION ("A large callback repeats a short loop") { loopProbe = 1; }
    SECTION ("A large callback repeats a short loop one frame off its head") { loopProbe = 2; }
    SECTION ("A large loop callback needs no head span") { loopProbe = 3; }
    SECTION ("Exactly 32768 loop-head frames are ready") { loopProbe = 4; }
    SECTION ("32769 loop-head frames remain unavailable") { loopProbe = 5; }
    SECTION ("A seek whose loop head never caches commits once its wait runs out") { loopProbe = 10; }
    SECTION ("A 128-frame loop masks every internal wrap") { loopProbe = 6; }
    SECTION ("A 127-frame loop keeps linear fallback") { loopProbe = 7; }
    SECTION ("A callback ending at a loop seam stays unmasked") { loopProbe = 8; }
    SECTION ("A huge loop length never overflows seam enumeration") { loopProbe = 9; }

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-loop-read-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

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
            buf.setSample (0, n, (float) n * kSlope);
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
    transport.setLoopRange (initialStart, kLoopEnd);
    transport.setLoopEnabled (initiallyEnabled);
    transport.setPlayhead (kLoopEnd - 200);
    transport.setState (Transport::State::Playing);

    if (trimmedFrames != 0)
    {
        preparedBlock = std::max (preparedBlock, 8192);
        auto& region = session.track (0).regions.front();
        region.timelineStart = 10000;
        region.sourceOffset = 777;
        region.lengthInSamples = std::min (trimmedFrames, kFileLen - 777);
        transport.setLoopEnabled (false);
        transport.setPlayhead (10000);
    }
    std::int64_t probeStart = 1000, probeEnd = 1256, probeTarget = 1000;
    if (loopProbe != 0)
    {
        preparedBlock = 65536;
        if (loopProbe == 2) ++probeTarget;
        if (loopProbe == 3) probeEnd = 200000;
        if (loopProbe == 4 || loopProbe == 5 || loopProbe == 10)
        {
            probeEnd = probeStart + (loopProbe == 4 ? 32768 : 32769);
            probeTarget = probeEnd - 128;
        }
        if (loopProbe == 6) probeEnd = probeStart + 128;
        if (loopProbe == 7) probeEnd = probeStart + 127;
        if (loopProbe == 8) preparedBlock = 256;
        if (loopProbe == 9)
        {
            probeStart = 0;
            probeEnd = std::numeric_limits<std::int64_t>::max();
            probeTarget = probeEnd - 100;
            preparedBlock = 128;
        }
        transport.setLoopRange (probeStart, probeEnd);
        transport.setLoopEnabled (true);
        transport.setPlayhead (probeTarget);
    }
    PlaybackEngine pe (session, false);
    pe.bindTransport (transport);
    pe.setSynchronousReadersForTest (trimmedFrames == 0 && loopProbe == 0);
    pe.prepare (preparedBlock);
    // Readiness is checked on its own; the wait that overrides it has sections of its own.
    if (! waitOut && loopProbe != 10)
        pe.setLocateWaitForTest (std::numeric_limits<std::int64_t>::max());
    pe.preparePlayback();

    if (loopProbe == 10)
    {
        // At 48 kHz a locate waits 14400 frames, which the first 65536-frame block outlasts.
        REQUIRE_FALSE (pe.locateReadyRt (probeTarget, preparedBlock, probeStart, probeEnd));
        REQUIRE (pe.locateReadyRt (probeTarget, preparedBlock, probeStart, probeEnd));
        REQUIRE_FALSE (pe.locateReadyRt (probeTarget - 1, preparedBlock, probeStart, probeEnd));
        return;
    }
    if (loopProbe != 0)
    {
        pe.locateReadyRt (probeTarget, preparedBlock, probeStart, probeEnd);
        for (int pass = 0; pass < 32; ++pass) pe.pumpReadersForTest();
        const bool ready = pe.locateReadyRt (probeTarget, preparedBlock, probeStart, probeEnd);
        if (loopProbe == 5)
        {
            REQUIRE_FALSE (ready);
            return;
        }
        REQUIRE (ready);
        std::vector<float> left ((size_t) preparedBlock), right ((size_t) preparedBlock);
        pe.readForTrack (0, probeTarget, left.data(), right.data(), preparedBlock, probeStart, probeEnd);
        const auto length = probeEnd - probeStart, firstTail = probeEnd - probeTarget;
        const bool split = length >= 128;
        constexpr double pi = 3.14159265358979323846;
        for (int n = 0; n < preparedBlock; ++n)
        {
            const auto frame = split && n >= firstTail
                ? probeStart + (n - firstTail) % length : probeTarget + n;
            double gain = 1.0;
            if (split && n >= firstTail && frame - probeStart < 64)
                gain *= std::pow (std::sin (pi * (double) (frame - probeStart) / 128.0), 2.0);
            const auto untilWrap = probeEnd - frame;
            if (split && untilWrap <= 64 && untilWrap < preparedBlock - n)
                gain *= std::pow (std::sin (pi * (double) untilWrap / 128.0), 2.0);
            const double expected = frame >= 0 && frame < kFileLen ? (double) frame * kSlope * gain : 0.0;
            INFO ("probe " << loopProbe << ", sample " << n);
            REQUIRE_THAT (left[(size_t) n], WithinAbs (expected, 2e-7));
            REQUIRE_THAT (right[(size_t) n], WithinAbs (expected, 2e-7));
        }
        return;
    }

    if (trimmedFrames != 0)
    {
        const auto target = trimmedFrames == kFileLen ? 170000 : 10000;
        const int block = preparedBlock == 16384 ? 16384 : 8192;
        if (waitOut)
        {
            // Nothing fills the readers here. The wait is 14400 frames at 48 kHz:
            // two 8192-frame blocks refuse, the third commits, and a new target
            // starts its own wait.
            REQUIRE_FALSE (pe.locateReadyRt (target, block, -1, -1));
            REQUIRE_FALSE (pe.locateReadyRt (target, block, -1, -1));
            REQUIRE (pe.locateReadyRt (target, block, -1, -1));
            REQUIRE_FALSE (pe.locateReadyRt (target + 1, block, -1, -1));
            pe.finishLocateBlockRt (target, false);
            REQUIRE_FALSE (pe.locateReadyRt (target, block, -1, -1));
            return;
        }
        REQUIRE_FALSE (pe.locateReadyRt (target, block, -1, -1));
        REQUIRE (transport.getCommittedPlayhead() == 10000);
        for (int pass = 0; pass < 32; ++pass) pe.pumpReadersForTest();
        REQUIRE (pe.locateReadyRt (target, block, -1, -1));
        std::vector<float> out ((size_t) block, -1.0f);
        pe.readForTrack (0, target, out.data(), nullptr, block);
        const auto& region = session.track (0).regions.front();
        for (int n = 0; n < block; ++n)
        {
            const auto at = target + n;
            const float expected = at < region.timelineStart + region.lengthInSamples
                ? (float) (777 + at - region.timelineStart) * kSlope : 0.0f;
            REQUIRE_THAT (out[(size_t) n], WithinAbs (expected, 2e-6f));
        }
        pe.holdLocateReadyForTest (true);
        REQUIRE_FALSE (pe.locateReadyRt (target, block, -1, -1));
        pe.prepare (preparedBlock);
        pe.setLocateWaitForTest (std::numeric_limits<std::int64_t>::max());
        REQUIRE (pe.locateReadyRt (target, block, -1, -1));
        pe.holdLocateReadyForTest (true);
        pe.stopPlayback();
        REQUIRE (pe.locateReadyRt (target, block, -1, -1));
        pe.preparePlayback();
        for (int pass = 0; pass < 32; ++pass) pe.pumpReadersForTest();
        pe.locateReadyRt (10000, block, -1, -1);
        for (int pass = 0; pass < 32; ++pass) pe.pumpReadersForTest();
        REQUIRE (pe.locateReadyRt (10000, block, -1, -1));
        return;
    }

    std::vector<float> tail (128);
    pe.readForTrack (0, kLoopEnd - 400, tail.data(), nullptr, 128);
    transport.setLoopRange (keepCaptureRange ? 50000 : kLoopStart, kLoopEnd);
    transport.setLoopEnabled (true);
    pe.beginAudioBlock (kLoopEnd - 200, kLoopStart, kLoopEnd);
    if (checkSeekReadiness)
        CHECK_FALSE (pe.locateReadyRt (kLoopEnd - 200, 400, kLoopStart, kLoopEnd));
    pe.service();

    std::vector<float> out (400, -1.0f);
    pe.readForTrack (0, kLoopEnd - 200, out.data(), nullptr, 400, kLoopStart, kLoopEnd);
    if (checkSeekReadiness)
        CHECK (pe.locateReadyRt (kLoopEnd - 200, 400, kLoopStart, kLoopEnd));

    // Pre-seam, outside the 64-sample fade-out window (which covers
    // offsets 136..199): raw pre-wrap source values.
    REQUIRE_THAT (out[0],   WithinAbs ((kLoopEnd - 200) * kSlope, 2e-4f));
    REQUIRE_THAT (out[100], WithinAbs ((kLoopEnd - 100) * kSlope, 2e-4f));

    // Post-seam, outside the fade-in window (offsets 200..263): wrapped
    // source values from loopStart — NOT the pre-wrap continuation.
    REQUIRE_THAT (out[300], WithinAbs ((kLoopStart + 100) * kSlope, 2e-4f));
    REQUIRE_THAT (out[399], WithinAbs ((kLoopStart + 199) * kSlope, 2e-4f));

    // Seam declick: the first wrapped sample is fully faded in by +64.
    REQUIRE (std::abs (out[200]) < 1e-4f);

    // Fade-out direction: attenuation deepens INTO the seam. The sample
    // adjacent to the wrap (offset 199) is silenced, the far edge of the
    // 64-sample window (offset 136) keeps full level, and every step in
    // between attenuates monotonically. An inverted ramp leaves the seam
    // discontinuity intact and swells backwards instead. Monotonicity holds
    // because the ramp changes little while the raised-cosine gain falls.
    REQUIRE (std::abs (out[199]) < 1e-3f);
    REQUIRE_THAT (out[136], WithinAbs ((kLoopEnd - 64) * kSlope, 2e-3f));
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

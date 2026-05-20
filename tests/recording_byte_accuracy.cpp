#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/RecordManager.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("focal-recording-accuracy-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}
} // namespace

// RecordManager is the path between the audio thread's writeInputBlock
// and a WAV on disk. A regression that mangles channel ordering, drops
// samples, or writes the wrong sample count is invisible at the
// transport-level UI — the user only finds out when they bounce and
// hear a click or a missing chunk. Tests below drive a known ramp
// through the public API and verify the finalised WAV reads back to
// the same values within float epsilon.

TEST_CASE ("RecordManager writes mono input back accurately",
           "[recording][recordmanager]")
{
    using focal::RecordManager;
    using focal::Session;
    using focal::Track;

    constexpr double kSampleRate = 48000.0;
    constexpr int    kNumBlocks  = 8;
    constexpr int    kBlockSize  = 256;
    constexpr int    kTotal      = kNumBlocks * kBlockSize;

    const auto dir = makeTempSessionDir();
    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    REQUIRE (rm.startRecording (kSampleRate, 0));

    // Push a known ramp: sample n carries value n * 1e-5f. Tiny scale
    // keeps every sample within [-1, +1] without saturating WAV's
    // 24-bit int range.
    std::vector<float> ramp ((size_t) kBlockSize, 0.0f);
    for (int b = 0; b < kNumBlocks; ++b)
    {
        for (int i = 0; i < kBlockSize; ++i)
            ramp[(size_t) i] = float (b * kBlockSize + i) * 1e-5f;
        rm.writeInputBlock (0, ramp.data(), nullptr, kBlockSize);
    }

    rm.stopRecording (kTotal);

    // stopRecording calls writers[t].reset() which destroys the
    // ThreadedWriter. JUCE's destructor blocks until the queue drains,
    // so the WAV is on disk by the time stopRecording returns — no
    // sleep needed (and CLAUDE.md test-style rules forbid them).

    // Find the produced WAV — RecordManager names it
    // track01_<stamp>.wav. We grab the first .wav in the audio dir.
    auto audioDir = session.getAudioDirectory();
    REQUIRE (audioDir.isDirectory());
    auto wavs = audioDir.findChildFiles (juce::File::findFiles, false, "*.wav");
    REQUIRE (wavs.size() == 1);

    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fmt.createReaderFor (wavs[0]));
    REQUIRE (reader != nullptr);
    REQUIRE (reader->numChannels == 1);
    REQUIRE ((int) reader->lengthInSamples == kTotal);

    juce::AudioBuffer<float> buf (1, kTotal);
    REQUIRE (reader->read (&buf, 0, kTotal, 0, true, false));
    for (int n = 0; n < kTotal; ++n)
    {
        const float expected = float (n) * 1e-5f;
        // 24-bit int round-trip noise floor ≈ 1 / 2^23 ≈ 1.2e-7.
        REQUIRE_THAT (buf.getSample (0, n), WithinAbs (expected, 2e-7f));
    }

    dir.deleteRecursively();
}

TEST_CASE ("RecordManager writes stereo L+R interleaving in order",
           "[recording][recordmanager]")
{
    using focal::RecordManager;
    using focal::Session;
    using focal::Track;

    constexpr double kSampleRate = 48000.0;
    constexpr int    kNumBlocks  = 4;
    constexpr int    kBlockSize  = 128;
    constexpr int    kTotal      = kNumBlocks * kBlockSize;

    const auto dir = makeTempSessionDir();
    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Stereo, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    REQUIRE (rm.startRecording (kSampleRate, 0));

    // L = +1e-4 * n, R = -1e-4 * n. Sign disagreement catches a swap
    // bug; per-channel ramp catches a stride-by-one bug.
    std::vector<float> L ((size_t) kBlockSize, 0.0f);
    std::vector<float> R ((size_t) kBlockSize, 0.0f);
    for (int b = 0; b < kNumBlocks; ++b)
    {
        for (int i = 0; i < kBlockSize; ++i)
        {
            const int n = b * kBlockSize + i;
            L[(size_t) i] =  float (n) * 1e-4f;
            R[(size_t) i] = -float (n) * 1e-4f;
        }
        rm.writeInputBlock (0, L.data(), R.data(), kBlockSize);
    }

    rm.stopRecording (kTotal);
    // ThreadedWriter destructor (in stopRecording's teardown loop)
    // drains the queue synchronously — see comment in the mono test.

    auto wavs = session.getAudioDirectory()
                    .findChildFiles (juce::File::findFiles, false, "*.wav");
    REQUIRE (wavs.size() == 1);

    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fmt.createReaderFor (wavs[0]));
    REQUIRE (reader != nullptr);
    REQUIRE (reader->numChannels == 2);
    REQUIRE ((int) reader->lengthInSamples == kTotal);

    juce::AudioBuffer<float> buf (2, kTotal);
    REQUIRE (reader->read (&buf, 0, kTotal, 0, true, true));
    for (int n = 0; n < kTotal; ++n)
    {
        REQUIRE_THAT (buf.getSample (0, n), WithinAbs ( float (n) * 1e-4f, 2e-7f));
        REQUIRE_THAT (buf.getSample (1, n), WithinAbs (-float (n) * 1e-4f, 2e-7f));
    }

    dir.deleteRecursively();
}

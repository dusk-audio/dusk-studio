#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "engine/DpAligner.h"

#include <cmath>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSr = 48000.0;

// A signal with distinctive onsets: short decaying tone bursts at irregular
// gaps so its onset envelope is unique (not periodic) -> a clean correlation
// peak. Returns `seconds` of audio.
std::vector<float> onsetySignal (double seconds, unsigned seed)
{
    const int n = (int) (seconds * kSr);
    std::vector<float> x ((size_t) n, 0.0f);
    std::mt19937 rng (seed);
    std::uniform_int_distribution<int> gap ((int) (0.15 * kSr), (int) (0.5 * kSr));
    std::uniform_real_distribution<float> freq (200.0f, 2000.0f);
    int pos = 0;
    while (pos < n)
    {
        const float f = freq (rng);
        const int blen = (int) (0.08 * kSr);
        for (int i = 0; i < blen && pos + i < n; ++i)
        {
            const float env = std::exp (-4.0f * (float) i / (float) blen);
            x[(size_t) (pos + i)] += 0.6f * env * std::sin (2.0f * juce::MathConstants<float>::pi
                                                             * f * (float) i / (float) kSr);
        }
        pos += gap (rng);
    }
    return x;
}

void addNoise (std::vector<float>& x, float amp, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> d (-amp, amp);
    for (auto& v : x) v += d (rng);
}

void writeWav (const juce::File& f, const std::vector<float>& mono)
{
    std::unique_ptr<juce::FileOutputStream> s (f.createOutputStream());
    REQUIRE (s != nullptr); REQUIRE (s->openedOk());
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (
        wav.createWriterFor (s.get(), kSr, 1, 24, {}, 0));
    REQUIRE (w != nullptr);
    s.release();
    juce::AudioBuffer<float> buf (1, (int) mono.size());
    std::copy (mono.begin(), mono.end(), buf.getWritePointer (0));
    REQUIRE (w->writeFromAudioSampleBuffer (buf, 0, (int) mono.size()));
    w.reset();
}

struct TempScope
{
    juce::File dir;
    TempScope()
    {
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-dpaligner-tests").getChildFile (juce::Uuid().toDashedString());
        if (dir.createDirectory().failed()) throw std::runtime_error ("temp dir");
    }
    ~TempScope() { dir.deleteRecursively(); }
};
} // namespace

TEST_CASE ("DpAligner: recovers a fragment's offset inside a mixdown", "[DpAligner]")
{
    using namespace duskstudio::dp;
    TempScope tmp;

    // Fragment of distinctive onsets; mix = quiet bed + the fragment placed at
    // a known offset + an unrelated decoy elsewhere.
    auto frag = onsetySignal (8.0, 1);
    const double trueStart = 47.0;            // seconds
    std::vector<float> mix ((size_t) (120.0 * kSr), 0.0f);
    addNoise (mix, 0.02f, 99);
    const int off = (int) (trueStart * kSr);
    for (size_t i = 0; i < frag.size() && off + (int) i < (int) mix.size(); ++i)
        mix[(size_t) (off + (int) i)] += 0.8f * frag[i];
    auto decoy = onsetySignal (8.0, 2);
    const int doff = (int) (10.0 * kSr);
    for (size_t i = 0; i < decoy.size() && doff + (int) i < (int) mix.size(); ++i)
        mix[(size_t) (doff + (int) i)] += 0.8f * decoy[i];

    const auto mixFile  = tmp.dir.getChildFile ("mix.wav");
    const auto fragFile = tmp.dir.getChildFile ("ZZ0000_1.wav");
    writeWav (mixFile, mix);
    writeWav (fragFile, frag);

    const auto res = alignToMixdown (mixFile, { fragFile });
    REQUIRE (res.size() == 1);
    REQUIRE (res[0].placed);
    REQUIRE_FALSE (res[0].fullLength);
    REQUIRE (res[0].dominance >= 1.5f);
    // 5 ms onset resolution; allow a few frames of slack.
    REQUIRE_THAT (res[0].positionSeconds, WithinAbs (trueStart, 0.1));
}

TEST_CASE ("DpAligner: rejects a fragment not present in the mix", "[DpAligner]")
{
    using namespace duskstudio::dp;
    TempScope tmp;

    std::vector<float> mix ((size_t) (60.0 * kSr), 0.0f);
    auto inMix = onsetySignal (6.0, 3);
    const int off = (int) (20.0 * kSr);
    for (size_t i = 0; i < inMix.size(); ++i) mix[(size_t) (off + (int) i)] += 0.8f * inMix[i];
    addNoise (mix, 0.02f, 7);

    auto absent = onsetySignal (6.0, 12345);   // never added to the mix
    const auto mixFile = tmp.dir.getChildFile ("mix.wav");
    const auto absFile = tmp.dir.getChildFile ("ZZ0001_1.wav");
    writeWav (mixFile, mix);
    writeWav (absFile, absent);

    const auto res = alignToMixdown (mixFile, { absFile });
    REQUIRE (res.size() == 1);
    // A take that isn't in the mix has no dominant peak -> left unplaced.
    REQUIRE_FALSE (res[0].placed);
}

TEST_CASE ("DpAligner: full-length take placed at song start", "[DpAligner]")
{
    using namespace duskstudio::dp;
    TempScope tmp;

    auto bed = onsetySignal (30.0, 5);
    const auto mixFile  = tmp.dir.getChildFile ("mix.wav");
    const auto fragFile = tmp.dir.getChildFile ("ZZ0002_1.wav");
    writeWav (mixFile, bed);
    writeWav (fragFile, bed);   // same length as mix -> full-length

    const auto res = alignToMixdown (mixFile, { fragFile });
    REQUIRE (res.size() == 1);
    REQUIRE (res[0].placed);
    REQUIRE (res[0].fullLength);
    REQUIRE (res[0].timelineStartSamples == 0);
}

TEST_CASE ("DpAligner: near-mix-length fragment is not falsely placed", "[DpAligner]")
{
    using namespace duskstudio::dp;
    TempScope tmp;
    // Fragment shorter than the mix (so it bypasses the full-length shortcut)
    // but within the dominance exclusion radius (~2 s): the whole valid lag
    // range sits inside the exclusion zone, so there is no competitor peak. The
    // aligner must report this as ambiguous (not placed), not a false positive.
    auto mix  = onsetySignal (30.0, 8);
    auto frag = onsetySignal (29.5, 9);   // 0.5 s shorter, unrelated content
    const auto mixFile  = tmp.dir.getChildFile ("mix.wav");
    const auto fragFile = tmp.dir.getChildFile ("ZZ0000_1.wav");
    writeWav (mixFile, mix);
    writeWav (fragFile, frag);

    const auto res = alignToMixdown (mixFile, { fragFile });
    REQUIRE (res.size() == 1);
    REQUIRE_FALSE (res[0].fullLength);
    REQUIRE_FALSE (res[0].placed);
}

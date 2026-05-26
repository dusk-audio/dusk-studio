#include <catch2/catch_test_macros.hpp>

#include "comp_helpers.h"
#include <vector>

using namespace duskstudio::comp_test;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlockSize  = 32;        // 0.67 ms resolution
constexpr double kSineHz     = 1000.0;

void configureCleanBaseline (UniversalCompressor& c)
{
    setParam   (c, "bypass",          0.0f);
    setParam   (c, "mix",             100.0f);
    setChoice  (c, "auto_makeup",     0);
    setChoice  (c, "saturation_mode", 2);
    setChoice  (c, "distortion_type", 0);
    setParam   (c, "sidechain_hp",    0.0f);
    setChoice  (c, "oversampling",    0);
    setParam   (c, "noise_enable",    0.0f);
    setParam   (c, "stereo_link",     100.0f);
}

// Drive `silenceSamples` of zero then `driveSamples` of a 1 kHz sine at `amp`,
// capture per-block peak-dB envelope of the compressor output.
std::vector<float> captureStepEnvelope (UniversalCompressor& c,
                                        int silenceSamples,
                                        int driveSamples,
                                        float amp)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> blk (2, kBlockSize);
    std::vector<float> envDb;
    envDb.reserve ((silenceSamples + driveSamples) / kBlockSize + 1);

    const double w = 2.0 * juce::MathConstants<double>::pi * kSineHz / kSampleRate;
    double phase = 0.0;
    int produced = 0;
    while (produced < silenceSamples + driveSamples)
    {
        const int n = juce::jmin (kBlockSize, silenceSamples + driveSamples - produced);
        blk.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            const int t = produced + i;
            const float s = (t < silenceSamples) ? 0.0f
                                                  : amp * (float) std::sin (phase);
            blk.setSample (0, i, s);
            blk.setSample (1, i, s);
            if (t >= silenceSamples) phase += w;
        }
        c.processBlock (blk, midi);

        float pk = 0.0f;
        for (int i = 0; i < n; ++i)
            pk = juce::jmax (pk, std::abs (blk.getSample (0, i)));
        envDb.push_back (juce::Decibels::gainToDecibels (pk, -120.0f));

        produced += n;
    }
    return envDb;
}

// Drive sine for `driveSamples` then silence for `silenceSamples`; capture envelope.
// Used for measuring release time (GR recovers back toward zero).
std::vector<float> captureReleaseEnvelope (UniversalCompressor& c,
                                           int driveSamples,
                                           int silenceSamples,
                                           float amp)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> blk (2, kBlockSize);
    std::vector<float> envDb;
    envDb.reserve ((silenceSamples + driveSamples) / kBlockSize + 1);

    const double w = 2.0 * juce::MathConstants<double>::pi * kSineHz / kSampleRate;
    double phase = 0.0;
    int produced = 0;
    while (produced < driveSamples + silenceSamples)
    {
        const int n = juce::jmin (kBlockSize, driveSamples + silenceSamples - produced);
        blk.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            const int t = produced + i;
            const float s = (t < driveSamples) ? amp * (float) std::sin (phase) : 0.0f;
            blk.setSample (0, i, s);
            blk.setSample (1, i, s);
            phase += w;
        }
        c.processBlock (blk, midi);

        float pk = 0.0f;
        for (int i = 0; i < n; ++i)
            pk = juce::jmax (pk, std::abs (blk.getSample (0, i)));
        envDb.push_back (juce::Decibels::gainToDecibels (pk, -120.0f));

        produced += n;
    }
    return envDb;
}

int blocksTo63 (const std::vector<float>& env, int startBlock, float startDb, float endDb)
{
    const float target = startDb + 0.63f * (endDb - startDb);
    const bool descending = endDb < startDb;
    for (int i = startBlock + 1; i < (int) env.size(); ++i)
    {
        if (descending ? (env[i] <= target) : (env[i] >= target))
            return i - startBlock;
    }
    return -1;
}

float blocksToMs (int blocks)
{
    return (blocks > 0) ? (float) (blocks * kBlockSize) * 1000.0f / (float) kSampleRate : 0.0f;
}
} // namespace

TEST_CASE ("VCA attack/release land in the expected order of magnitude", "[compressor][vca][timing]")
{
    auto comp = makeComp (kSampleRate, kBlockSize);
    configureCleanBaseline (*comp);
    setChoice (*comp, "mode",          (int) CompressorMode::VCA);
    setParam  (*comp, "vca_threshold", -20.0f);
    setParam  (*comp, "vca_ratio",     10.0f);
    setParam  (*comp, "vca_attack",    5.0f);
    setParam  (*comp, "vca_release",   200.0f);
    setParam  (*comp, "vca_output",    0.0f);

    const int silence = (int) (kSampleRate * 0.10);   // 100 ms
    const int drive   = (int) (kSampleRate * 0.30);   // 300 ms
    const auto env = captureStepEnvelope (*comp, silence, drive, 0.7f);

    const int silenceBlocks = silence / kBlockSize;
    REQUIRE (env.size() > (size_t) silenceBlocks + 4);

    const float startDb = env[silenceBlocks + 1];
    const float endDb   = env.back();
    INFO ("VCA env startDb=" << startDb << " endDb=" << endDb);
    REQUIRE (startDb - endDb > 3.0f);   // compression actually happened

    const int blocks = blocksTo63 (env, silenceBlocks + 1, startDb, endDb);
    const float measuredMs = blocksToMs (blocks);
    INFO ("VCA attack measured=" << measuredMs << " ms (target ~5 ms, program-dependent)");
    REQUIRE (measuredMs > 0.0f);
    REQUIRE (measuredMs < 50.0f);   // generous upper bound: program-dependent attack varies
}

TEST_CASE ("VCA release recovers within configured time order", "[compressor][vca][timing]")
{
    auto comp = makeComp (kSampleRate, kBlockSize);
    configureCleanBaseline (*comp);
    setChoice (*comp, "mode",          (int) CompressorMode::VCA);
    setParam  (*comp, "vca_threshold", -20.0f);
    setParam  (*comp, "vca_ratio",     10.0f);
    setParam  (*comp, "vca_attack",    1.0f);
    setParam  (*comp, "vca_release",   150.0f);
    setParam  (*comp, "vca_output",    0.0f);

    const int drive   = (int) (kSampleRate * 0.30);   // 300 ms loud (settle GR)
    const int silence = (int) (kSampleRate * 0.40);   // 400 ms tail
    const auto env = captureReleaseEnvelope (*comp, drive, silence, 0.7f);

    const int driveBlocks = drive / kBlockSize;
    REQUIRE (env.size() > (size_t) driveBlocks + 4);

    const float settledDb = env[driveBlocks - 1];   // end of drive
    // After silence the output rests at noise floor; envelope rebounds back
    // toward unity only at the moment of any tail; the meaningful measurement
    // is how quickly the *gain reduction* recovers — i.e. the envelope is now
    // a residual of the comp's recovering gain × zero input = zero. We instead
    // measure recovery by running a brief verification ping right after the
    // tail and confirming gain reduction has decayed enough that the ping
    // passes near unity. Here we approximate: drive a fresh short impulse at
    // 200 ms after drop and verify it passes the comp largely uncompressed.
    auto verifyPing = [&] (float postDropMs) -> float
    {
        auto c2 = makeComp (kSampleRate, kBlockSize);
        configureCleanBaseline (*c2);
        setChoice (*c2, "mode",          (int) CompressorMode::VCA);
        setParam  (*c2, "vca_threshold", -20.0f);
        setParam  (*c2, "vca_ratio",     10.0f);
        setParam  (*c2, "vca_attack",    1.0f);
        setParam  (*c2, "vca_release",   150.0f);
        setParam  (*c2, "vca_output",    0.0f);
        const int dropDrive   = (int) (kSampleRate * 0.30);
        const int waitSilence = (int) (kSampleRate * (postDropMs / 1000.0));
        const int ping        = (int) (kSampleRate * 0.020);  // 20 ms

        juce::MidiBuffer midi;
        juce::AudioBuffer<float> blk (2, kBlockSize);
        const double w = 2.0 * juce::MathConstants<double>::pi * kSineHz / kSampleRate;
        double phase = 0.0;
        float pingPeak = 0.0f;
        const int total = dropDrive + waitSilence + ping;
        int produced = 0;
        while (produced < total)
        {
            const int n = juce::jmin (kBlockSize, total - produced);
            blk.setSize (2, n, false, false, true);
            for (int i = 0; i < n; ++i)
            {
                const int t = produced + i;
                float s = 0.0f;
                if      (t < dropDrive)                 s = 0.7f * (float) std::sin (phase);
                else if (t < dropDrive + waitSilence)   s = 0.0f;
                else                                    s = 0.7f * (float) std::sin (phase);
                blk.setSample (0, i, s);
                blk.setSample (1, i, s);
                phase += w;
            }
            c2->processBlock (blk, midi);
            for (int i = 0; i < n; ++i)
            {
                const int t = produced + i;
                if (t >= dropDrive + waitSilence)
                    pingPeak = juce::jmax (pingPeak, std::abs (blk.getSample (0, i)));
            }
            produced += n;
        }
        return juce::Decibels::gainToDecibels (pingPeak, -120.0f);
    };

    const float earlyPing = verifyPing (10.0);    // 10 ms — still squashed
    const float latePing = verifyPing (500.0);   // 500 ms — comp should have released

    INFO ("VCA early ping=" << earlyPing << " dB  late ping=" << latePing << " dB");
    INFO ("VCA settled (drive end) env=" << settledDb << " dB");
    REQUIRE (latePing > earlyPing + 1.5f);   // later ping passes louder → release happened
}

TEST_CASE ("FET attack lands in the expected order of magnitude", "[compressor][fet][timing]")
{
    // FET (1176) output peaks are normalised by the input transformer and
    // output stage, so the absolute peak drop after envelope settles is
    // smaller than the analytic GR would suggest. We measure timing relative
    // to whatever drop the donor actually produces.
    auto comp = makeComp (kSampleRate, kBlockSize);
    configureCleanBaseline (*comp);
    setChoice (*comp, "mode",         (int) CompressorMode::FET);
    setChoice (*comp, "fet_ratio",    2);     // 12:1
    setParam  (*comp, "fet_input",    15.0f); // drive well into compression
    setParam  (*comp, "fet_attack",   1.0f);
    setParam  (*comp, "fet_release",  100.0f);
    setParam  (*comp, "fet_output",   0.0f);

    const int silence = (int) (kSampleRate * 0.10);
    const int drive   = (int) (kSampleRate * 0.20);
    const auto env = captureStepEnvelope (*comp, silence, drive, 0.5f);

    const int silenceBlocks = silence / kBlockSize;
    REQUIRE (env.size() > (size_t) silenceBlocks + 4);

    const float startDb = env[silenceBlocks + 1];
    const float endDb   = env.back();
    INFO ("FET env startDb=" << startDb << " endDb=" << endDb);
    REQUIRE (startDb - endDb > 0.4f);   // envelope must move at all (post-transformer)

    const int blocks = blocksTo63 (env, silenceBlocks + 1, startDb, endDb);
    const float measuredMs = blocksToMs (blocks);
    INFO ("FET attack measured=" << measuredMs << " ms (target ~1 ms, program-dependent)");
    REQUIRE (measuredMs > 0.0f);
    REQUIRE (measuredMs < 30.0f);
}

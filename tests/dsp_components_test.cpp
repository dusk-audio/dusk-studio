// M1 / M7 narrow-link DSP tests.
//
// These are intentionally JUCE-only (no Dusk Studio DSP classes + no
// donor link). They validate the project-wide smoother + filter
// invariants Dusk Studio relies on without dragging the channel-strip
// translation unit + the multi-comp donor objects into the test link.
//
// Covered today:
//   1. juce::SmoothedValue ramp duration is sample-rate-independent
//      when reset via .reset(sampleRate, rampSeconds). The contract
//      Dusk Studio's whole UI relies on (M7) — 20 ms of smoothing
//      means 20 ms at any device rate.
//   2. juce::dsp::IIR::Filter survives prepare → process → re-prepare
//      at a radically different sample rate without leaking NaN /
//      infinity into its output (the H8 idempotency net for any
//      future migration to runtime SR changes).
//
// Bigger-scope tests (Pultec EQ, Tape Saturation curves, Bus Comp
// transfer function) live in the donor — they'd require linking
// ../plugins-main/plugins/* into this test binary the way
// comp_static_curve.cpp does. Tracked as a follow-up because the
// link set is non-trivial and out of this phase's 4-file scope.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <cmath>

namespace
{
// Walk a smoother forward by `samples` ticks + return its final value.
// Mirrors the per-sample drain pattern Dusk Studio's ChannelStrip uses
// inside processAndAccumulate.
float drainSmoother (juce::SmoothedValue<float>& sv, int samples) noexcept
{
    float v = 0.0f;
    for (int i = 0; i < samples; ++i)
        v = sv.getNextValue();
    return v;
}
} // namespace

TEST_CASE ("SmoothedValue.reset(SR, secs) is sample-rate-independent",
           "[dsp][smoother][m7]")
{
    constexpr double  kRampSecs = 0.020;     // 20 ms — Dusk Studio's standard
    constexpr float   kTarget   = 1.0f;

    // 44.1 kHz: 20 ms = 882 samples.
    juce::SmoothedValue<float> svLow;
    svLow.reset (44100.0, kRampSecs);
    svLow.setCurrentAndTargetValue (0.0f);
    svLow.setTargetValue (kTarget);
    const int samplesLow = (int) std::lround (44100.0 * kRampSecs);
    const float vLow = drainSmoother (svLow, samplesLow);

    // 96 kHz: 20 ms = 1920 samples.
    juce::SmoothedValue<float> svHigh;
    svHigh.reset (96000.0, kRampSecs);
    svHigh.setCurrentAndTargetValue (0.0f);
    svHigh.setTargetValue (kTarget);
    const int samplesHigh = (int) std::lround (96000.0 * kRampSecs);
    const float vHigh = drainSmoother (svHigh, samplesHigh);

    // Both should land at (or essentially at) the target after their
    // respective sample counts. JUCE's SmoothedValue lands on target
    // when its remaining step count hits zero; both above use the same
    // 20 ms wall-clock budget at different rates, so the end value
    // matches at the tolerance of one float step.
    using Catch::Matchers::WithinAbs;
    REQUIRE_THAT ((double) vLow,  WithinAbs (kTarget, 1.0e-4));
    REQUIRE_THAT ((double) vHigh, WithinAbs (kTarget, 1.0e-4));

    // Halfway through the ramp, the two smoothers should produce
    // closely-matching outputs — proving the ramp duration is
    // sample-rate-independent (not raw-sample-count-based).
    juce::SmoothedValue<float> svMidLow;
    svMidLow.reset (44100.0, kRampSecs);
    svMidLow.setCurrentAndTargetValue (0.0f);
    svMidLow.setTargetValue (kTarget);
    juce::SmoothedValue<float> svMidHigh;
    svMidHigh.reset (96000.0, kRampSecs);
    svMidHigh.setCurrentAndTargetValue (0.0f);
    svMidHigh.setTargetValue (kTarget);

    const float halfLow  = drainSmoother (svMidLow,  samplesLow  / 2);
    const float halfHigh = drainSmoother (svMidHigh, samplesHigh / 2);
    // 2% tolerance covers the discrete-step asymmetry at 0.5 ramp
    // position (samplesHigh/2 = 960 vs samplesLow/2 = 441; both land
    // near the midpoint but JUCE rounds the step count to int).
    REQUIRE_THAT ((double) halfLow, WithinAbs ((double) halfHigh, 0.02));
}

TEST_CASE ("IIR::Filter survives re-prepare at radically different SR/BS",
           "[dsp][filter][h8]")
{
    // Coefficients are sample-rate-dependent; a re-prepare on a filter
    // that's already pumping audio is the M2 / H8 stress scenario the
    // ChannelStrip's prepare() / .reset() pair has to survive. Without
    // a clean state wipe between SR steps, IIR filters can integrate
    // their stored z^-1 / z^-2 history at a now-wrong rate and bloom
    // into NaN within a handful of samples.

    using FilterT = juce::dsp::IIR::Filter<float>;
    using CoeffsT = juce::dsp::IIR::Coefficients<float>;

    FilterT filter;

    auto runBlock = [&] (double sampleRate, int blockSize,
                           float* dataInOut, int numSamples)
    {
        // Re-derive coefficients for the current rate; resonant
        // low-shelf is a deliberately edgy choice (high Q exaggerates
        // any stale-state ringing). Q chosen so a 44 -> 96 kHz jump
        // produces visibly-different coefficients.
        auto coeffs = CoeffsT::makeLowShelf (sampleRate, 200.0f,
                                                 1.4f /*Q*/, 1.5f /*gain*/);
        filter.coefficients = coeffs;

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = sampleRate;
        spec.maximumBlockSize = (juce::uint32) blockSize;
        spec.numChannels      = 1;
        filter.prepare (spec);
        // Wipe z^-N state so the new coefficients integrate from zero
        // rather than the prior SR's stored history.
        filter.reset();

        juce::dsp::AudioBlock<float> block (&dataInOut, 1, (std::size_t) numSamples);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        filter.process (ctx);
    };

    // First pass: 44.1 kHz / 256-sample blocks. Drive with a unit
    // impulse + zeros so the filter has real history before the
    // re-prepare.
    constexpr int kNumSamples = 1024;
    std::vector<float> buffer ((std::size_t) kNumSamples, 0.0f);
    buffer[0] = 1.0f;  // impulse
    runBlock (44100.0, 256, buffer.data(), kNumSamples);

    // Verify the first pass produced finite output (sanity).
    for (float v : buffer) REQUIRE (std::isfinite (v));

    // Second pass: 96 kHz / 2048-sample blocks. A different SR + a
    // different blockSize than the first prepare. New impulse to drive
    // fresh response.
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    buffer[0] = 1.0f;
    runBlock (96000.0, 2048, buffer.data(), kNumSamples);

    // Verify the re-prepared filter's output stayed finite throughout.
    // Any NaN / inf here would indicate state was not properly reset
    // between SR steps — the exact failure mode ChannelStrip::prepare
    // guards against via its explicit .reset() chain (H8).
    for (float v : buffer) REQUIRE (std::isfinite (v));
}

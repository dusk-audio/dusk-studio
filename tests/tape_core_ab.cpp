#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "PluginProcessor.h"            // JUCE donor (ground truth)
#include <core/TapeMachineDSP.hpp>      // duskaudio:: JUCE-free core

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// A/B parity: duskaudio::TapeMachineDSP against the JUCE TapeMachineAudioProcessor
// it was transcribed from, driven the way MasterBus drives it (all 16 live params
// pushed before the render, in-place stereo processing).
//
// At 1x the port's oversampler is a transparent passthrough, so the two chains
// are the same arithmetic and the null is exact. At 2x/4x the port swaps JUCE's
// equiripple half-band FIR for the shared polyphase half-band (core PORT_NOTES
// 3.1) - a deliberate re-voice, so only a bounded residual is expected there.
//
// Wow / flutter / noise stay at zero throughout: their generators are seeded
// from std::random_device in the donor and from fixed constants in the port
// (PORT_NOTES 3.5), so any nonzero setting is unnullable by construction.

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 256;
// Silence blocks before the measured signal: the donor's input/output
// juce::dsp::Gain stages start at unity and ramp to target over 20 ms * factor,
// while the port snaps them in prepare(). Feeding silence until both have
// settled removes the difference without perturbing any state (silence in ->
// silence out, and the hysteresis loop stays at the origin).
constexpr int    kWarmupBlocks = 32;
constexpr int    kSignalBlocks = 24;

struct Rng
{
    std::uint32_t s;
    float next() noexcept
    {
        s = s * 1664525u + 1013904223u;
        return (float) ((s >> 8) & 0xFFFFFF) / (float) 0x1000000 * 2.0f - 1.0f;
    }
};

// Deterministic stereo program: sustained partials, 0.11 s level steps and
// periodic transients, so the hysteresis loop, the head bump and the soft
// limiter all see real movement rather than a steady sine. withTransients=false
// drops the clicks; see the 2x/4x test for why that matters there.
void makeSignal (std::vector<float>& L, std::vector<float>& R, int total,
                 bool withTransients)
{
    L.assign ((size_t) total, 0.0f);
    R.assign ((size_t) total, 0.0f);
    Rng rng { 0x1234567u };

    constexpr double kTwoPi = 6.28318530717958647692;
    double p1 = 0.0, p2 = 0.0, p3 = 0.0;
    const int clickPeriod = std::max (1, (int) (kSampleRate * 0.09));
    const int clickLen    = std::max (1, (int) (kSampleRate * 0.0008));
    const float segAmp[5] = { 0.12f, 0.5f, 0.25f, 0.7f, 0.05f };

    for (int i = 0; i < total; ++i)
    {
        const double t = (double) i / kSampleRate;
        const float a = segAmp[(int) (t / 0.11) % 5];

        const float sL = 0.6f * (float) std::sin (p1) + 0.3f * (float) std::sin (p2)
                       + 0.2f * (float) std::sin (p3);
        const float sR = 0.55f * (float) std::sin (p1 + 0.7) + 0.32f * (float) std::sin (p2 * 1.01)
                       + 0.18f * (float) std::sin (p3);
        p1 += kTwoPi * 90.0 / kSampleRate;
        p2 += kTwoPi * 610.0 / kSampleRate;
        p3 += kTwoPi * 2350.0 / kSampleRate;

        float clk = 0.0f;
        if (withTransients && (i % clickPeriod) < clickLen)
            clk = rng.next() > 0.0f ? 0.55f : -0.55f;

        L[(size_t) i] = a * sL + clk;
        R[(size_t) i] = a * sR + clk * 0.8f;
    }
}

void makeInput (std::vector<float>& L, std::vector<float>& R, bool withTransients)
{
    const int warmup = kWarmupBlocks * kBlock;
    const int total  = warmup + kSignalBlocks * kBlock;
    std::vector<float> sigL, sigR;
    makeSignal (sigL, sigR, total - warmup, withTransients);
    L.assign ((size_t) total, 0.0f);
    R.assign ((size_t) total, 0.0f);
    std::copy (sigL.begin(), sigL.end(), L.begin() + warmup);
    std::copy (sigR.begin(), sigR.end(), R.begin() + warmup);
}

// The donor starts a juce::Timer in its constructor (host latency reporting),
// which needs a live MessageManager. Leaked on purpose: tearing the initialiser
// down between test cases would race that timer thread.
void ensureMessageLoop()
{
    static auto* keepAlive = new juce::ScopedJuceInitialiser_GUI();
    (void) keepAlive;
}

void setParam (juce::AudioProcessorValueTreeState& apvts, const char* id, float value)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (id));
    REQUIRE (p != nullptr);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
}

struct Settings
{
    int   machine = 0, speed = 1, type = 0, signalPath = 0, eqStandard = 0, calibration = 0;
    float inputGainDb = 0.0f, bias = 50.0f, highpassHz = 20.0f, lowpassHz = 20000.0f;
    float noiseAmount = 0.0f, wow = 0.0f, flutter = 0.0f, outputGainDb = 0.0f;
    bool  autoCal = true, autoComp = true;
};

void applyToDonor (TapeMachineAudioProcessor& proc, const Settings& s, int osChoice)
{
    auto& apvts = proc.getAPVTS();
    setParam (apvts, "tapeMachine",   (float) s.machine);
    setParam (apvts, "tapeSpeed",     (float) s.speed);
    setParam (apvts, "tapeType",      (float) s.type);
    setParam (apvts, "signalPath",    (float) s.signalPath);
    setParam (apvts, "eqStandard",    (float) s.eqStandard);
    setParam (apvts, "calibration",   (float) s.calibration);
    setParam (apvts, "inputGain",     s.inputGainDb);
    setParam (apvts, "bias",          s.bias);
    setParam (apvts, "highpassFreq",  s.highpassHz);
    setParam (apvts, "lowpassFreq",   s.lowpassHz);
    setParam (apvts, "noiseAmount",   s.noiseAmount);
    setParam (apvts, "wowAmount",     s.wow);
    setParam (apvts, "flutterAmount", s.flutter);
    setParam (apvts, "outputGain",    s.outputGainDb);
    setParam (apvts, "autoCal",       s.autoCal ? 1.0f : 0.0f);
    setParam (apvts, "autoComp",      s.autoComp ? 1.0f : 0.0f);
    setParam (apvts, "oversampling",  (float) osChoice);
}

void applyToCore (duskaudio::TapeMachineDSP& core, const Settings& s, int osChoice)
{
    core.setTapeMachine  (s.machine);
    core.setTapeSpeed    (s.speed);
    core.setTapeType     (s.type);
    core.setSignalPath   (s.signalPath);
    core.setEqStandard   (s.eqStandard);
    core.setCalibration  (s.calibration);
    core.setInputGainDb  (s.inputGainDb);
    core.setBias         (s.bias);
    core.setHighpassHz   (s.highpassHz);
    core.setLowpassHz    (s.lowpassHz);
    core.setNoiseAmount  (s.noiseAmount);
    core.setWow          (s.wow);
    core.setFlutter      (s.flutter);
    core.setOutputGainDb (s.outputGainDb);
    core.setAutoCal      (s.autoCal);
    core.setAutoComp     (s.autoComp);
    core.setOversampling (osChoice);
}

struct Render
{
    std::vector<float> L, R;
    int latency = 0;
};

Render renderDonor (const Settings& s, int osChoice,
                    const std::vector<float>& inL, const std::vector<float>& inR)
{
    ensureMessageLoop();
    TapeMachineAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, kSampleRate, kBlock);
    applyToDonor (proc, s, osChoice);
    proc.prepareToPlay (kSampleRate, kBlock);

    Render out;
    out.L = inL;
    out.R = inR;
    out.latency = proc.getLatencySamples();

    juce::AudioBuffer<float> buf (2, kBlock);
    juce::MidiBuffer midi;
    for (int off = 0; off < (int) inL.size(); off += kBlock)
    {
        const int n = std::min (kBlock, (int) inL.size() - off);
        buf.setSize (2, n, false, false, true);
        buf.copyFrom (0, 0, out.L.data() + off, n);
        buf.copyFrom (1, 0, out.R.data() + off, n);
        midi.clear();
        proc.processBlock (buf, midi);
        std::copy_n (buf.getReadPointer (0), n, out.L.data() + off);
        std::copy_n (buf.getReadPointer (1), n, out.R.data() + off);
    }
    return out;
}

Render renderCore (const Settings& s, int osChoice,
                   const std::vector<float>& inL, const std::vector<float>& inR)
{
    duskaudio::TapeMachineDSP core;
    applyToCore (core, s, osChoice);
    core.prepare (kSampleRate, kBlock);
    core.reset();

    Render out;
    out.L = inL;
    out.R = inR;
    out.latency = core.latencySamples();

    for (int off = 0; off < (int) inL.size(); off += kBlock)
    {
        const int n = std::min (kBlock, (int) inL.size() - off);
        float* lr[2] = { out.L.data() + off, out.R.data() + off };
        core.processBlock (lr, lr, 2, n);
    }
    return out;
}

// Peak |donor - core| over the measured region, each side shifted by its own
// reported latency.
float peakResidual (const Render& donor, const Render& core, int firstSample)
{
    const int total = (int) donor.L.size();
    const int lead  = std::max (donor.latency, core.latency) + 1;
    float worst = 0.0f;
    for (int i = firstSample; i + lead < total; ++i)
    {
        worst = std::max (worst, std::abs (donor.L[(size_t) (i + donor.latency)]
                                         - core.L[(size_t) (i + core.latency)]));
        worst = std::max (worst, std::abs (donor.R[(size_t) (i + donor.latency)]
                                         - core.R[(size_t) (i + core.latency)]));
    }
    return worst;
}

// Same, but resampling the core onto the donor's timeline. The two half-band
// designs do not share a group delay and neither delay lands on a whole sample
// (the port's 4x latency is literally 26.5), so an integer shift leaves a
// sub-sample offset that dominates everything else - it alone accounts for
// -25 dB of residual-to-signal at 2x versus -48 dB once removed. The window is
// searched rather than hard-coded so this stays a residual measurement; the
// latency values themselves are pinned by their own test below.
float peakResidualSubSample (const Render& donor, const Render& core, int firstSample)
{
    const int total = (int) donor.L.size();
    const int lead  = std::max (donor.latency, core.latency) + 2;
    const double centre = core.latency - donor.latency;
    float best = std::numeric_limits<float>::max();

    for (int k = -30; k <= 30; ++k)
    {
        const double shift = centre + k * 0.05;
        const int    base  = (int) std::floor (shift);
        const float  frac  = (float) (shift - base);
        float worst = 0.0f;
        for (int i = firstSample; i + lead < total; ++i)
        {
            const size_t j = (size_t) (i + base);
            const float cL = core.L[j] * (1.0f - frac) + core.L[j + 1] * frac;
            const float cR = core.R[j] * (1.0f - frac) + core.R[j + 1] * frac;
            worst = std::max (worst, std::abs (donor.L[(size_t) i] - cL));
            worst = std::max (worst, std::abs (donor.R[(size_t) i] - cR));
        }
        best = std::min (best, worst);
    }
    return best;
}
} // namespace

using Catch::Matchers::WithinAbs;

TEST_CASE ("TapeMachineDSP nulls against the JUCE donor at 1x", "[tape][ab]")
{
    std::vector<float> inL, inR;
    makeInput (inL, inR, true);

    Settings s;
    // Measured peak |diff|: 2e-7 .. 5e-7 for every setting below except hot
    // drive, where the float-vs-double coefficient ULP the port documents
    // (PORT_NOTES 2a) gets amplified through the hysteresis loop to 8e-6.
    float tol = 1.0e-6f;
    SECTION ("unity drive")        { s.inputGainDb = 0.0f; }
    SECTION ("hot drive")          { s.inputGainDb = 9.0f; tol = 2.0e-5f; }
    SECTION ("manual output gain") { s.autoComp = false; s.outputGainDb = -4.0f; }
    SECTION ("Classic 102, 30 IPS, CCIR")
    {
        s.machine = 1; s.speed = 2; s.eqStandard = 1; s.type = 2;
    }
    SECTION ("Sync path, manual bias, tone filters engaged")
    {
        s.signalPath = 1; s.autoCal = false; s.bias = 72.0f;
        s.highpassHz = 80.0f; s.lowpassHz = 12000.0f;
    }
    SECTION ("Input path, +6 dB calibration") { s.signalPath = 2; s.calibration = 2; }

    const auto donor = renderDonor (s, 0, inL, inR);
    const auto core  = renderCore  (s, 0, inL, inR);

    REQUIRE (donor.latency == 0);
    REQUIRE (core.latency == 0);
    REQUIRE_THAT (peakResidual (donor, core, kWarmupBlocks * kBlock), WithinAbs (0.0f, tol));
}

TEST_CASE ("TapeMachineDSP tracks the JUCE donor within tolerance at 2x and 4x", "[tape][ab]")
{
    // Bounded residual, not a null: the half-band designs differ (PORT_NOTES
    // 3.1). Measured sub-sample-aligned peak |diff| on this program - 6.2e-3 at
    // 2x, 7.0e-3 at 4x, i.e. -48 dB and -51 dB residual-to-signal. Tolerance is
    // ~2x the measurement so a real regression in the tape chain shows up
    // without the test being knife-edge.
    //
    // Transients are deliberately off here. The port's half-band is the shorter
    // filter (23 vs 49 base-rate samples of latency at 2x) with a correspondingly
    // wider transition band, so click energy near Nyquist is treated completely
    // differently by the two - a bare up/down round trip through them differs by
    // -9 dB on click-laden material with no tape in the loop at all. That is the
    // documented re-voice, not something this test can bound usefully.
    std::vector<float> inL, inR;
    makeInput (inL, inR, false);

    Settings s;
    int osChoice = 1;
    SECTION ("2x") { osChoice = 1; }
    SECTION ("4x") { osChoice = 2; }

    const auto donor = renderDonor (s, osChoice, inL, inR);
    const auto core  = renderCore  (s, osChoice, inL, inR);

    REQUIRE_THAT (peakResidualSubSample (donor, core, kWarmupBlocks * kBlock),
                  WithinAbs (0.0f, 1.5e-2f));
}

TEST_CASE ("TapeMachineDSP latency is stable per oversampling factor", "[tape][ab]")
{
    // The port does NOT inherit the donor's latency: JUCE's equiripple half-band
    // reports 0 / 49 / 60 base-rate samples at 1x / 2x / 4x, the port's polyphase
    // half-band 0 / 23 / 27 (26.5 rounded). MasterBus sizes its tape-crossfade
    // dry delay from the core's own report, so the master stays internally
    // consistent - but anything that assumed the donor's numbers is stale.
    ensureMessageLoop();
    const int expectedCore[]  = { 0, 23, 27 };
    const int expectedDonor[] = { 0, 49, 60 };

    for (int osChoice = 0; osChoice <= 2; ++osChoice)
    {
        Settings s;
        TapeMachineAudioProcessor proc;
        proc.setPlayConfigDetails (2, 2, kSampleRate, kBlock);
        applyToDonor (proc, s, osChoice);
        proc.prepareToPlay (kSampleRate, kBlock);

        duskaudio::TapeMachineDSP core;
        applyToCore (core, s, osChoice);
        core.prepare (kSampleRate, kBlock);

        INFO ("oversampling choice " << osChoice);
        REQUIRE (core.latencySamples() == expectedCore[osChoice]);
        REQUIRE (proc.getLatencySamples() == expectedDonor[osChoice]);
    }
}

TEST_CASE ("TapeMachineDSP passes silence through as silence", "[tape]")
{
    for (int osChoice = 0; osChoice <= 2; ++osChoice)
    {
        Settings s;
        s.inputGainDb = 9.0f;   // heavy drive: the tape core must still stay at rest
        duskaudio::TapeMachineDSP core;
        applyToCore (core, s, osChoice);
        core.prepare (kSampleRate, kBlock);
        core.reset();

        std::vector<float> L ((size_t) kBlock, 0.0f), R ((size_t) kBlock, 0.0f);
        for (int b = 0; b < 16; ++b)
        {
            float* lr[2] = { L.data(), R.data() };
            core.processBlock (lr, lr, 2, kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                INFO ("oversampling choice " << osChoice << ", block " << b << ", sample " << i);
                REQUIRE_THAT (L[(size_t) i], WithinAbs (0.0f, 1.0e-9f));
                REQUIRE_THAT (R[(size_t) i], WithinAbs (0.0f, 1.0e-9f));
            }
        }
    }
}

TEST_CASE ("TapeMachineDSP bypass is a sample-exact passthrough", "[tape]")
{
    std::vector<float> sigL, sigR;
    makeSignal (sigL, sigR, kBlock * 4, true);

    Settings s;
    s.inputGainDb = 6.0f;
    duskaudio::TapeMachineDSP core;
    applyToCore (core, s, 2);
    core.prepare (kSampleRate, kBlock);
    core.reset();
    core.setBypass (true);

    auto L = sigL, R = sigR;
    for (int off = 0; off < (int) sigL.size(); off += kBlock)
    {
        float* lr[2] = { L.data() + off, R.data() + off };
        core.processBlock (lr, lr, 2, kBlock);
    }

    for (size_t i = 0; i < sigL.size(); ++i)
    {
        INFO ("sample " << i);
        REQUIRE (L[i] == sigL[i]);
        REQUIRE (R[i] == sigR[i]);
    }
}

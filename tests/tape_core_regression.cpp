#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <core/TapeMachineDSP.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Tone regression for duskaudio::TapeMachineDSP, the Tape Machine 2 core the
// master bus and the built-in colour insert both run.
//
// This file used to A/B the core against the JUCE TapeMachineAudioProcessor it
// was transcribed from. That comparison ended when the core moved to Tape
// Machine 2: the JUCE shell still carries the v1 DSP, so a null against it would
// be asserting that v2 sounds like v1, which is the opposite of what the move
// was for. What replaces it is a drift guard. The expectations below were
// measured from this core, so they do not prove it is right; they prove it has
// not changed since the revision that was listened to and accepted.
//
// The measures are aggregate rather than sample-exact on purpose. A checksum of
// floats does not survive a different compiler, a different libm or a different
// architecture, and this suite runs on GCC, Clang, MSVC, x86_64 and arm64.
//
// Wow, flutter and noise stay at zero: their generators are free-running, so
// nothing about them is reproducible across a render boundary.

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 256;
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
// limiter all see real movement rather than a steady sine.
void makeSignal (std::vector<float>& L, std::vector<float>& R, int total)
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
        if ((i % clickPeriod) < clickLen)
            clk = rng.next() > 0.0f ? 0.55f : -0.55f;

        L[(size_t) i] = a * sL + clk;
        R[(size_t) i] = a * sR + clk * 0.8f;
    }
}

void makeInput (std::vector<float>& L, std::vector<float>& R)
{
    const int warmup = kWarmupBlocks * kBlock;
    const int total  = warmup + kSignalBlocks * kBlock;
    std::vector<float> sigL, sigR;
    makeSignal (sigL, sigR, total - warmup);
    L.assign ((size_t) total, 0.0f);
    R.assign ((size_t) total, 0.0f);
    std::copy (sigL.begin(), sigL.end(), L.begin() + warmup);
    std::copy (sigR.begin(), sigR.end(), R.begin() + warmup);
}

struct Settings
{
    int   machine = 0, speed = 1, type = 0, signalPath = 0, eqStandard = 0, calibration = 0;
    float inputGainDb = 0.0f, bias = 50.0f, highpassHz = 20.0f, lowpassHz = 20000.0f;
    float noiseAmount = 0.0f, wow = 0.0f, flutter = 0.0f, outputGainDb = 0.0f;
    bool  autoCal = true, autoComp = true;
};

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

struct Measure
{
    double peak = 0.0;
    double rms = 0.0;
};

// Measured over the signal region only, past the warmup and past the reported
// latency, so neither the settling nor the group delay is in the numbers.
Measure measure (const Render& r)
{
    const int first = kWarmupBlocks * kBlock + r.latency;
    const int last  = (int) r.L.size();
    Measure m;
    double sum = 0.0;
    int n = 0;
    for (int i = first; i < last; ++i)
    {
        const double a = std::abs ((double) r.L[(size_t) i]);
        const double b = std::abs ((double) r.R[(size_t) i]);
        m.peak = std::max (m.peak, std::max (a, b));
        sum += a * a + b * b;
        n += 2;
    }
    m.rms = n > 0 ? std::sqrt (sum / (double) n) : 0.0;
    return m;
}
} // namespace

TEST_CASE ("TapeMachineDSP latency is stable per oversampling factor", "[tape][regression]")
{
    // The local anti-aliasing stages around the core's nonlinearities dominate
    // the round trip and are expressed in base-rate samples, so the figure does
    // not move with the factor. MasterBus sizes its tape-crossfade dry delay
    // from this report, so a change here is a change to the master's alignment.
    for (int osChoice = 0; osChoice <= 2; ++osChoice)
    {
        duskaudio::TapeMachineDSP core;
        Settings s;
        applyToCore (core, s, osChoice);
        core.prepare (kSampleRate, kBlock);

        INFO ("oversampling choice " << osChoice);
        REQUIRE (core.latencySamples() == 56);
    }
}

TEST_CASE ("TapeMachineDSP holds its tone across a fixed render", "[tape][regression]")
{
    std::vector<float> inL, inR;
    makeInput (inL, inR);

    const auto rendered = renderCore (Settings {}, /*osChoice*/ 1, inL, inR);
    const auto m = measure (rendered);

    // Measured from this core. A failure here is a tone change: either it was
    // intended, and these move with it in the same commit, or it was not.
    INFO ("peak " << m.peak << " rms " << m.rms);
    REQUIRE_THAT (m.peak, Catch::Matchers::WithinRel (0.679183, 0.002));
    REQUIRE_THAT (m.rms,  Catch::Matchers::WithinRel (0.107245, 0.002));
}

TEST_CASE ("TapeMachineDSP ignores the oversampling choice", "[tape][regression]")
{
    // The core's own factorFromChoice is hardwired to 2x: the local
    // anti-aliasing stages around each nonlinearity do the work, so the outer
    // factor has nothing left to do and the setter is inert by design. The
    // master bus and the built-in colour insert both hand it the engine's
    // global factor, so this pins that handing it a different one changes
    // nothing rather than silently re-voicing the master.
    std::vector<float> inL, inR;
    makeInput (inL, inR);

    const auto at1x = renderCore (Settings {}, 0, inL, inR);
    for (int osChoice = 1; osChoice <= 2; ++osChoice)
    {
        const auto other = renderCore (Settings {}, osChoice, inL, inR);
        INFO ("oversampling choice " << osChoice);
        REQUIRE (other.latency == at1x.latency);
        REQUIRE (other.L.size() == at1x.L.size());
        for (size_t i = 0; i < at1x.L.size(); ++i)
        {
            REQUIRE (other.L[i] == at1x.L[i]);
            REQUIRE (other.R[i] == at1x.R[i]);
        }
    }
}

TEST_CASE ("TapeMachineDSP passes silence through as silence", "[tape]")
{
    duskaudio::TapeMachineDSP core;
    Settings s;
    applyToCore (core, s, 1);
    core.prepare (kSampleRate, kBlock);
    core.reset();

    std::vector<float> l ((size_t) kBlock, 0.0f), r ((size_t) kBlock, 0.0f);
    for (int b = 0; b < 64; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        float* lr[2] = { l.data(), r.data() };
        core.processBlock (lr, lr, 2, kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            REQUIRE (std::isfinite (l[(size_t) i]));
            REQUIRE_THAT (l[(size_t) i], Catch::Matchers::WithinAbs (0.0, 1e-7));
            REQUIRE_THAT (r[(size_t) i], Catch::Matchers::WithinAbs (0.0, 1e-7));
        }
    }
}

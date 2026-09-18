#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <FourKEQDSP.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Tone regression for duskaudio::FourKEQDSP, the console EQ every channel strip
// and every bus runs.
//
// The A/B against the JUCE BritishEQProcessor next door is a sanity bound, not a
// null: the two are different revisions of the same design and their difference
// is measured in tenths of a dB, so it cannot tell a deliberate re-voicing from
// an accident. This file is the drift guard. The expectations were measured from
// this core at the revision that was listened to and accepted, so they do not
// prove it is right; they prove it has not moved since.
//
// Aggregate measures rather than a checksum: a float checksum does not survive a
// different compiler, libm or architecture, and this suite runs on GCC, Clang,
// MSVC, x86_64 and arm64.

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 256;
constexpr int    kWarmupBlocks = 32;
constexpr int    kSignalBlocks = 24;

// The always-on console drive the channel strip pushes into every EQ instance.
constexpr float kConsoleSaturationDrive = 22.0f;

struct Rng
{
    std::uint32_t s;
    float next() noexcept
    {
        s = s * 1664525u + 1013904223u;
        return (float) ((s >> 8) & 0xFFFFFF) / (float) 0x1000000 * 2.0f - 1.0f;
    }
};

// Deterministic stereo program: sustained partials across the band the EQ
// shapes, level steps, and periodic transients so the saturator sees real
// movement rather than a steady sine.
void makeSignal (std::vector<float>& L, std::vector<float>& R, int total)
{
    L.assign ((size_t) total, 0.0f);
    R.assign ((size_t) total, 0.0f);
    Rng rng { 0x1234567u };

    constexpr double kTwoPi = 6.28318530717958647692;
    double p1 = 0.0, p2 = 0.0, p3 = 0.0, p4 = 0.0;
    const int clickPeriod = std::max (1, (int) (kSampleRate * 0.09));
    const int clickLen    = std::max (1, (int) (kSampleRate * 0.0008));
    const float segAmp[5] = { 0.12f, 0.5f, 0.25f, 0.7f, 0.05f };

    for (int i = 0; i < total; ++i)
    {
        const double t = (double) i / kSampleRate;
        const float a = segAmp[(int) (t / 0.11) % 5];

        const float sL = 0.5f * (float) std::sin (p1) + 0.3f * (float) std::sin (p2)
                       + 0.2f * (float) std::sin (p3) + 0.1f * (float) std::sin (p4);
        const float sR = 0.46f * (float) std::sin (p1 + 0.7) + 0.32f * (float) std::sin (p2 * 1.01)
                       + 0.18f * (float) std::sin (p3) + 0.09f * (float) std::sin (p4 * 0.99);
        p1 += kTwoPi * 55.0 / kSampleRate;
        p2 += kTwoPi * 420.0 / kSampleRate;
        p3 += kTwoPi * 3100.0 / kSampleRate;
        p4 += kTwoPi * 9500.0 / kSampleRate;

        float clk = 0.0f;
        if ((i % clickPeriod) < clickLen)
            clk = rng.next() > 0.0f ? 0.55f : -0.55f;

        L[(size_t) i] = 0.7f * (a * sL + clk);
        R[(size_t) i] = 0.7f * (a * sR + clk * 0.8f);
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

// One dialled-in strip: filters in, both mids cut and lifted, both shelves up.
void applyToCore (duskaudio::FourKEQDSP& eq, bool black)
{
    eq.setOversampling (0);
    eq.setMsMode (false);
    eq.setAutoGain (false);
    eq.setBypass (false);
    eq.setEqType (black ? 1 : 0);
    eq.setHpfEnabled (true);   eq.setHpfFreq (80.0f);
    eq.setLpfEnabled (true);   eq.setLpfFreq (16000.0f);
    eq.setLfGain (4.0f);   eq.setLfFreq (100.0f);   eq.setLfBell (false);
    eq.setLmGain (-3.0f);  eq.setLmFreq (400.0f);   eq.setLmQ (1.2f);
    eq.setHmGain (2.5f);   eq.setHmFreq (3000.0f);  eq.setHmQ (0.8f);
    eq.setHfGain (3.0f);   eq.setHfFreq (10000.0f); eq.setHfBell (false);
    eq.setInputGainDb (0.0f);
    eq.setOutputGainDb (0.0f);
    eq.setSaturation (kConsoleSaturationDrive);
}

struct Render
{
    std::vector<float> L, R;
    int latency = 0;
};

Render renderCore (bool black, const std::vector<float>& inL, const std::vector<float>& inR)
{
    duskaudio::FourKEQDSP eq;
    applyToCore (eq, black);
    eq.prepare (kSampleRate, kBlock);
    eq.reset();

    Render out;
    out.L = inL;
    out.R = inR;
    out.latency = eq.getLatencySamples();

    for (int off = 0; off < (int) inL.size(); off += kBlock)
    {
        const int n = std::min (kBlock, (int) inL.size() - off);
        float* lr[2] = { out.L.data() + off, out.R.data() + off };
        eq.processBlock (lr, lr, 2, n);
    }
    return out;
}

struct Measure
{
    double peak = 0.0;
    double rms = 0.0;
};

Measure measure (const Render& r)
{
    const int first = kWarmupBlocks * kBlock + r.latency;
    Measure m;
    double sum = 0.0;
    int n = 0;
    for (int i = first; i < (int) r.L.size(); ++i)
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

TEST_CASE ("FourKEQDSP reports no latency at the rate the strips run it", "[eq][regression]")
{
    // The strips drive the core at 1x and do their own oversampling around it,
    // so it must add nothing for delay compensation to carry.
    duskaudio::FourKEQDSP eq;
    applyToCore (eq, false);
    eq.prepare (kSampleRate, kBlock);
    REQUIRE (eq.getLatencySamples() == 0);
}

TEST_CASE ("FourKEQDSP holds its tone across a fixed render", "[eq][regression]")
{
    std::vector<float> inL, inR;
    makeInput (inL, inR);

    // Measured from this core. A failure is a tone change: either it was
    // intended, and these move with it in the same commit, or it was not.
    //
    // RMS is the sensitive one and carries the tighter bound: it moves with the
    // band gains and with the saturator's drive, so a tenth of a percent trips
    // on about 0.01 dB of shift anywhere in the curve. Peak at two tenths is
    // the guard on gross scaling and on clipping the transients, which is what
    // RMS alone would miss. Neither is tight enough to trip on the
    // fused-multiply-add and libm differences between the compilers and
    // architectures this suite runs on.
    SECTION ("brown")
    {
        const auto m = measure (renderCore (false, inL, inR));
        INFO ("peak " << m.peak << " rms " << m.rms);
        REQUIRE_THAT (m.peak, Catch::Matchers::WithinRel (0.704084, 0.002));
        REQUIRE_THAT (m.rms,  Catch::Matchers::WithinRel (0.0847519, 0.001));
    }

    SECTION ("black")
    {
        const auto m = measure (renderCore (true, inL, inR));
        INFO ("peak " << m.peak << " rms " << m.rms);
        REQUIRE_THAT (m.peak, Catch::Matchers::WithinRel (0.783426, 0.002));
        REQUIRE_THAT (m.rms,  Catch::Matchers::WithinRel (0.0882388, 0.001));
    }
}

TEST_CASE ("the flat image a bypassed strip pushes is safe", "[eq][regression]")
{
    // A strip with its EQ section off leaves every band and filter at the
    // value-init zeros and keeps running the core for its always-on console
    // character, so zero frequencies and a zero Q reach the designers. The
    // output has to stay finite and stay at level: the tone shaping is what
    // bypasses, not the strip.
    std::vector<float> inL, inR;
    makeInput (inL, inR);

    duskaudio::FourKEQDSP eq;
    eq.setOversampling (0);
    eq.setMsMode (false);
    eq.setAutoGain (false);
    eq.setBypass (false);
    eq.setHpfEnabled (false); eq.setHpfFreq (0.0f);
    eq.setLpfEnabled (false); eq.setLpfFreq (0.0f);
    eq.setLfGain (0.0f); eq.setLfFreq (0.0f); eq.setLfBell (false);
    eq.setLmGain (0.0f); eq.setLmFreq (0.0f); eq.setLmQ (0.0f);
    eq.setHmGain (0.0f); eq.setHmFreq (0.0f); eq.setHmQ (0.0f);
    eq.setHfGain (0.0f); eq.setHfFreq (0.0f); eq.setHfBell (false);
    eq.setEqType (0);
    eq.setSaturation (kConsoleSaturationDrive);
    eq.setInputGainDb (0.0f);
    eq.setOutputGainDb (0.0f);
    eq.prepare (kSampleRate, kBlock);
    eq.reset();

    Render out;
    out.L = inL;
    out.R = inR;
    for (int off = 0; off < (int) inL.size(); off += kBlock)
    {
        const int n = std::min (kBlock, (int) inL.size() - off);
        float* lr[2] = { out.L.data() + off, out.R.data() + off };
        eq.processBlock (lr, lr, 2, n);
    }
    bool finite = true;
    for (float v : out.L)
        finite = finite && std::isfinite (v);
    REQUIRE (finite);

    Render dry;
    dry.L = inL;
    dry.R = inR;
    const auto wet = measure (out);
    const auto in  = measure (dry);
    INFO ("in rms " << in.rms << " out rms " << wet.rms);
    REQUIRE (wet.rms > in.rms * 0.5);    // no collapse
    REQUIRE (wet.rms < in.rms * 1.5);    // no runaway
}

TEST_CASE ("FourKEQDSP passes silence through as silence", "[eq]")
{
    // The console saturator's noise floor is switched off in the core's prepare,
    // so a silent strip stays digitally silent through an always-on saturation
    // stage. The revision before 4K EQ 2 dithered a floor in unconditionally.
    duskaudio::FourKEQDSP eq;
    applyToCore (eq, false);
    eq.prepare (kSampleRate, kBlock);
    eq.reset();

    std::vector<float> l ((size_t) kBlock, 0.0f), r ((size_t) kBlock, 0.0f);
    for (int b = 0; b < 64; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        float* lr[2] = { l.data(), r.data() };
        eq.processBlock (lr, lr, 2, kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            REQUIRE (std::isfinite (l[(size_t) i]));
            REQUIRE_THAT (l[(size_t) i], Catch::Matchers::WithinAbs (0.0, 1e-9));
            REQUIRE_THAT (r[(size_t) i], Catch::Matchers::WithinAbs (0.0, 1e-9));
        }
    }
}

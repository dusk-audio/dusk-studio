// Tests for the tape Jiles-Atherton hysteresis model (real loop, not a static
// waveshaper). What matters for shipping this on the master:
//   1. it can never go unstable / NaN, even at absurd input + drive (the clamp
//      backstop holds) — AND it is naturally bounded at normal levels (the clamp
//      is not what keeps it in range),
//   2. it actually has memory: the output at a MATCHED input level differs on the
//      rising vs the falling edge (and that difference collapses when the loop is
//      turned off),
//   3. the bias-linearised blend preserves the anhysteretic small-signal gain so
//      the nominal master tone is preserved, and collapses to the pure
//      anhysteretic curve at full bias,
//   4. the loop's susceptibility (langevinDeriv) is the true derivative of the
//      anhysteretic curve (langevin) the model actually uses.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ImprovedTapeEmulation.h"   // JilesAthertonHysteresis is header-only

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr float kTwoPi = 6.28318530718f;

// Type 456, precision deck. {Ms, a, alpha, k, c}
JilesAthertonHysteresis makeJA (JilesAthertonHysteresis::TapeFormulationParams p
                                    = { 280.0f, 720.0f, 0.016f, 640.0f, 0.50f })
{
    JilesAthertonHysteresis ja;
    ja.setFormulation (p);
    ja.prepare (192000.0, 4);
    ja.reset();
    return ja;
}

// RMS of a buffer's settled tail.
float rmsTail (const std::vector<float>& v, int from)
{
    double acc = 0.0; int n = 0;
    for (int i = from; i < (int) v.size(); ++i) { acc += (double) v[(size_t) i] * v[(size_t) i]; ++n; }
    return (float) std::sqrt (acc / std::max (1, n));
}

// Drive a sine, return the steady-state gain (RMS out / RMS in).
float steadyGain (JilesAthertonHysteresis& ja, float amp, float drive, float bias,
                  float hz = 1000.0f, int total = 192000)
{
    const float w = kTwoPi * hz / 192000.0f;
    std::vector<float> in ((size_t) total), out ((size_t) total);
    for (int i = 0; i < total; ++i)
    {
        in[(size_t) i]  = amp * std::sin (w * (float) i);
        out[(size_t) i] = ja.processSample (in[(size_t) i], drive, bias);
    }
    return rmsTail (out, total / 2) / rmsTail (in, total / 2);
}
} // namespace

TEST_CASE ("Tape J-A clamp backstop holds under absurd input and drive", "[tape][hysteresis]")
{
    // This is a backstop test: the input is deliberately absurd so the internal
    // magnetisation slams the ±1.5·Ms clamp. It proves no-NaN / no-runaway, not
    // natural stability (see the benign-level test for that).
    JilesAthertonHysteresis ja = makeJA();
    float maxAbs = 0.0f;
    for (int i = 0; i < 300000; ++i)
    {
        float in = 9.0f * std::sin (0.07f * (float) i)
                 + (((i / 977) % 2) ? 6.0f : -6.0f)
                 + ((i % 5000 == 0) ? 50.0f : 0.0f);
        const float out = ja.processSample (in, 3.0f, 0.0f);
        REQUIRE (std::isfinite (out));
        maxAbs = std::max (maxAbs, std::abs (out));
    }
    REQUIRE (maxAbs < 100.0f);   // bounded — no runaway
}

TEST_CASE ("Tape J-A is naturally stable at normal level (clamp not engaged)", "[tape][hysteresis]")
{
    // At a normal-to-hot level the integrator must stay in range on its own —
    // the clamp must NOT be the thing holding the output bounded. A regression
    // that makes the ODE diverge under real signal would be masked by the clamp
    // in the absurd-input test, but caught here.
    JilesAthertonHysteresis ja = makeJA();
    const float w = kTwoPi * 1000.0f / 192000.0f;
    float maxAbs = 0.0f;
    for (int i = 0; i < 96000; ++i)
    {
        const float out = ja.processSample (0.5f * std::sin (w * (float) i), 1.5f, 0.5f);
        REQUIRE (std::isfinite (out));
        maxAbs = std::max (maxAbs, std::abs (out));
    }
    REQUIRE (maxAbs < 0.5f);   // well under any clamp-driven bound
}

TEST_CASE ("Tape J-A traces a hysteresis loop (output depends on history)", "[tape][hysteresis]")
{
    // Capture the output at the SAME input level (+0.3) on the rising vs falling
    // edge by interpolating to the exact crossing — a wide capture window would
    // measure the curve's slope (input mismatch), not memory.
    auto loopOpening = [] (JilesAthertonHysteresis& ja) -> float
    {
        const float w = kTwoPi * 300.0f / 192000.0f;
        // Settle the loop; keep the LAST sample's in/out as the measurement's
        // prev. (Re-processing i=7999 separately would advance the JA state an
        // extra step for the same input and skew the loop opening.)
        float prevIn = 0.0f, prevOut = 0.0f;
        for (int i = 0; i < 8000; ++i)
        {
            prevIn  = 0.6f * std::sin (w * (float) i);
            prevOut = ja.processSample (prevIn, 1.8f, 0.5f);
        }

        const int   cycle = (int) (kTwoPi / w) + 2;
        float risingOut = 0.0f, fallingOut = 0.0f;
        bool  gotR = false, gotF = false;
        for (int i = 8000; i < 8000 + cycle; ++i)
        {
            const float in  = 0.6f * std::sin (w * (float) i);
            const float out = ja.processSample (in, 1.8f, 0.5f);
            if ((prevIn < 0.3f) != (in < 0.3f))    // crossed +0.3 this step
            {
                const float frac = (0.3f - prevIn) / (in - prevIn);
                const float at   = prevOut + frac * (out - prevOut);
                if (in > prevIn) { risingOut  = at; gotR = true; }
                else             { fallingOut = at; gotF = true; }
            }
            prevIn = in; prevOut = out;
        }
        REQUIRE (gotR);
        REQUIRE (gotF);
        return std::abs (risingOut - fallingOut);
    };

    SECTION ("the loop is open with real hysteresis params")
    {
        JilesAthertonHysteresis ja = makeJA();
        REQUIRE (loopOpening (ja) > 5.0e-3f);
    }

    SECTION ("the loop closes when reversibility = 1 (control)")
    {
        // c = 1, k = 0, alpha = 0 → fully anhysteretic, no memory. The matched
        // capture must then read ~zero, proving the test responds to the
        // hysteresis term and not to capture geometry.
        JilesAthertonHysteresis ja = makeJA ({ 280.0f, 720.0f, 0.0f, 0.0f, 1.0f });
        REQUIRE (loopOpening (ja) < 1.0e-4f);
    }
}

TEST_CASE ("Tape J-A small-signal gain matches the designed blend across bias", "[tape][hysteresis]")
{
    // This raw stage is voiced, NOT unity: the loop path's small-signal gain is
    // the reversibility c, so the blend sits at 1 - loopWeight*(1-c) with
    // loopWeight = (1-bias)/2. The deck's auto-makeup restores unity at the full
    // chain (the engine self-test audits that end-to-end). Here we pin the blend
    // math so a regression in it can't hide behind a loose "near unity" tolerance.
    const float c = 0.5f;
    for (float bias : { 0.0f, 0.5f, 1.0f })
    {
        JilesAthertonHysteresis ja = makeJA();
        const float g        = steadyGain (ja, 0.01f, 1.0f, bias);
        const float loopW    = (1.0f - bias) * 0.5f;
        const float expected = 1.0f - loopW * (1.0f - c);   // 0.75 / 0.875 / 1.0
        INFO ("bias " << bias << ": gain=" << g << " expected=" << expected);
        REQUIRE_THAT (g, WithinAbs (expected, 0.01f));
    }

    // Whatever the absolute level, the small-signal gain must be FLAT across the
    // low range (no level-dependent compression below the knee) — that flatness,
    // not an absolute value, is what "nominal tone preserved" means here.
    JilesAthertonHysteresis a = makeJA(), b = makeJA();
    const float gLow  = steadyGain (a, 0.03f, 1.0f, 0.5f);
    const float gOper = steadyGain (b, 0.12f, 1.0f, 0.5f);
    INFO ("flatness: gain(0.03)=" << gLow << "  gain(0.12)=" << gOper);
    REQUIRE_THAT (gOper, WithinAbs (gLow, 0.02f));
}

TEST_CASE ("Tape J-A collapses to the anhysteretic curve at full bias", "[tape][hysteresis]")
{
    // bias = 1.0 → loopWeight = 0 → the output must be exactly the pure
    // anhysteretic (Langevin) path, the model's loop-free anchor.
    JilesAthertonHysteresis ja = makeJA();
    const float drive = 1.0f;
    const float biasFactor   = 1.0f + 0.6f * (0.5f - 1.0f);   // 0.7 at bias 1.0
    const float machineFactor = 1.08f;                        // default (not precision)
    const float eDrive = drive * biasFactor * machineFactor;
    const float fieldScale = 3200.0f;

    const float w = kTwoPi * 500.0f / 192000.0f;
    for (int i = 0; i < 2000; ++i)
    {
        const float in = 0.2f * std::sin (w * (float) i);
        const float out = ja.processSample (in, drive, 1.0f);
        const float H = in * eDrive * fieldScale;
        const float anhyst = 3.0f * JilesAthertonHysteresis::langevin (H / 720.0f) * 720.0f
                           / (eDrive * fieldScale);
        REQUIRE_THAT (out, WithinAbs (anhyst, 1.0e-5f));
    }
}

TEST_CASE ("Tape langevinDeriv is the true derivative of langevin", "[tape][hysteresis]")
{
    // The J-A loop's susceptibility must be the analytic derivative of the SAME
    // (Padé) anhysteretic curve the model evaluates — not of the exact coth−1/x.
    const float h = 1.0e-3f;
    for (float q = -5.4f; q <= 5.4f; q += 0.1f)
    {
        // Skip the Padé→asymptote blend knots, where langevin has a slope kink
        // and a centred difference straddling it would be meaningless.
        if (std::abs (std::abs (q) - 1.5f) < 0.05f) continue;
        if (std::abs (std::abs (q) - 2.5f) < 0.05f) continue;

        const float fd = (JilesAthertonHysteresis::langevin (q + h)
                        - JilesAthertonHysteresis::langevin (q - h)) / (2.0f * h);
        const float an = JilesAthertonHysteresis::langevinDeriv (q);
        INFO ("q = " << q << "  finite-diff = " << fd << "  analytic = " << an);
        REQUIRE_THAT (an, WithinAbs (fd, 3.0e-3f));
    }
}

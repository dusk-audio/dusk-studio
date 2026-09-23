#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/BusToneEq.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using Catch::Matchers::WithinAbs;
using duskstudio::BusToneEq;

namespace
{
constexpr double kPi = 3.14159265358979323846;

double bandDb (BusToneEq::Band band, double fs, double gainDb, double hz)
{
    return BusToneEq::magnitudeDb (BusToneEq::bandCoeffs (band, fs, gainDb), fs, hz);
}

double hpfDb (double fs, double cornerHz, double hz)
{
    return BusToneEq::magnitudeDb (BusToneEq::hpfCoeffs (fs, cornerHz), fs, hz);
}

// The RBJ cookbook s-domain prototypes the bands are matched to, in dB.
double analogBandDb (BusToneEq::Band band, double gainDb, double hz)
{
    const double A = std::pow (10.0, gainDb / 40.0);
    if (band == BusToneEq::Mid)
    {
        const double x = hz / BusToneEq::kMidHz, Q = BusToneEq::kMidQ;
        const double u = (1.0 - x * x) * (1.0 - x * x), n = x * A / Q, d = x / (A * Q);
        return 10.0 * std::log10 ((u + n * n) / (u + d * d));
    }
    const bool high = band == BusToneEq::High;
    const double x = hz / (high ? BusToneEq::kHighHz : BusToneEq::kLowHz), x2 = x * x;
    const double Q = BusToneEq::kShelfQ;
    const double mid = (A / (Q * Q)) * x2, hi = (1.0 - A * x2) * (1.0 - A * x2), lo = (A - x2) * (A - x2);
    return 10.0 * std::log10 (high ? A * A * (hi + mid) / (lo + mid) : A * A * (lo + mid) / (hi + mid));
}

std::vector<float> sine (double fs, double hz, float amp, int n)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i)
        v[(size_t) i] = amp * (float) std::sin (2.0 * kPi * hz * (double) i / fs);
    return v;
}

// Runs `in` through the EQ in 256-sample blocks, switching from `before` to
// `after` at sample `switchAt` (a block boundary is placed there).
std::vector<float> run (BusToneEq& eq, const std::vector<float>& in,
                        const BusToneEq::Targets& before, const BusToneEq::Targets& after,
                        int switchAt)
{
    std::vector<float> L = in, R = in;
    int offset = 0;
    while (offset < (int) in.size())
    {
        int n = std::min (256, (int) in.size() - offset);
        if (offset < switchAt && offset + n > switchAt)
            n = switchAt - offset;
        eq.setTargets (offset < switchAt ? before : after);
        eq.process (L.data() + offset, R.data() + offset, n);
        offset += n;
    }
    return L;
}

double rmsDb (const std::vector<float>& v, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i)
        s += (double) v[(size_t) i] * (double) v[(size_t) i];
    return 10.0 * std::log10 (std::max (s / (double) (to - from), 1.0e-30));
}

// Peak of the content above 10 kHz (4th-order Butterworth highpass), dBFS. A
// click from a stepped coefficient change is broadband and shows up here; a
// 1 kHz tone and its smooth gain envelope do not.
double clickPeakDb (const std::vector<float>& x, double fs, int from, int to)
{
    struct Section
    {
        double b0, b1, b2, a1, a2, z1 = 0.0, z2 = 0.0;
        double process (double v)
        {
            const double y = b0 * v + z1;
            z1 = b1 * v - a1 * y + z2;
            z2 = b2 * v - a2 * y;
            return y;
        }
    };
    const auto make = [fs] (double Q)
    {
        const double k = std::tan (kPi * 10000.0 / fs), k2 = k * k, n = 1.0 / (1.0 + k / Q + k2);
        return Section { n, -2.0 * n, n, 2.0 * (k2 - 1.0) * n, (1.0 - k / Q + k2) * n };
    };
    Section s1 = make (0.54119610), s2 = make (1.30656296);
    double peak = 0.0;
    for (int i = 0; i < (int) x.size(); ++i)
    {
        const double y = s2.process (s1.process (x[(size_t) i]));
        if (i >= from && i < to)
            peak = std::max (peak, std::abs (y));
    }
    return 20.0 * std::log10 (std::max (peak, 1.0e-12));
}

BusToneEq::Targets flat()
{
    BusToneEq::Targets t;
    t.eqOn = true;
    return t;
}
} // namespace

TEST_CASE ("BusToneEq: each band plays its spec gain at its frequency", "[BusToneEq][bus]")
{
    for (const double fs : { 44100.0, 48000.0, 96000.0 })
    {
        CAPTURE (fs);
        for (const double g : { 9.0, -9.0 })
        {
            CAPTURE (g);
            // The bell reads its full gain at its centre.
            CHECK_THAT (bandDb (BusToneEq::Mid, fs, g, 800.0), WithinAbs (g, 0.01));
            // A shelf's frequency is its half-gain point; its gain is the plateau.
            CHECK_THAT (bandDb (BusToneEq::Low, fs, g, 300.0), WithinAbs (g / 2.0, 0.01));
            CHECK_THAT (bandDb (BusToneEq::Low, fs, g, 10.0), WithinAbs (g, 0.01));
            CHECK_THAT (bandDb (BusToneEq::High, fs, g, 2000.0), WithinAbs (g / 2.0, 0.01));
            CHECK_THAT (bandDb (BusToneEq::High, fs, g, 20000.0), WithinAbs (g, 0.01));
        }
    }

    SECTION ("the processed signal carries the same gain")
    {
        BusToneEq eq;
        eq.prepare (48000.0);
        auto boost = flat();
        boost.gainDb = { 0.0f, 9.0f, 0.0f };
        const auto in = sine (48000.0, 800.0, 0.1f, 48000);
        const auto out = run (eq, in, boost, boost, 0);
        CHECK_THAT (rmsDb (out, 24000, 48000) - rmsDb (in, 24000, 48000), WithinAbs (9.0, 0.02));
    }
}

TEST_CASE ("BusToneEq: shelves rise monotonically with no overshoot", "[BusToneEq][bus]")
{
    // Slope S = 1: the steepest shelf that does not bump past its plateau or
    // dip below 0 dB on the other side.
    for (const auto band : { BusToneEq::Low, BusToneEq::High })
    {
        double lowest = 1.0e9, highest = -1.0e9, previous = band == BusToneEq::Low ? 1.0e9 : -1.0e9;
        bool monotonic = true;
        for (double hz = 10.0; hz < 22000.0; hz *= 1.01)
        {
            const double db = bandDb (band, 48000.0, 9.0, hz);
            lowest = std::min (lowest, db);
            highest = std::max (highest, db);
            monotonic = monotonic && (band == BusToneEq::Low ? db <= previous + 1.0e-9 : db >= previous - 1.0e-9);
            previous = db;
        }
        CAPTURE ((int) band);
        CHECK (monotonic);
        CHECK (highest <= 9.0 + 1.0e-3);
        CHECK (lowest >= -1.0e-3);
    }
}

TEST_CASE ("BusToneEq: bands keep the analog shape up to Nyquist", "[BusToneEq][bus]")
{
    // Matched design: no cramping at the native rate. A bilinear bell misses
    // by 0.044 dB at 44.1 kHz; this one stays inside 0.02 dB.
    for (const double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        for (const auto band : { BusToneEq::Low, BusToneEq::Mid, BusToneEq::High })
            for (const double g : { -9.0, -4.5, 4.5, 9.0 })
            {
                double worst = 0.0;
                const double top = std::min (20000.0, 0.499 * fs);
                for (double hz = 20.0; hz <= top; hz *= 1.005)
                    worst = std::max (worst, std::abs (bandDb (band, fs, g, hz) - analogBandDb (band, g, hz)));
                CAPTURE (fs, (int) band, g);
                CHECK (worst < 0.02);
            }
}

TEST_CASE ("BusToneEq: the highpass is -3 dB at its corner and falls at 12 dB/oct", "[BusToneEq][bus]")
{
    for (const double fs : { 44100.0, 48000.0, 96000.0 })
        for (const double fc : { 20.0, 100.0, 1000.0, 3000.0 })
        {
            CAPTURE (fs, fc);
            CHECK_THAT (hpfDb (fs, fc, fc), WithinAbs (-3.0103, 0.005));
            CHECK_THAT (hpfDb (fs, fc, fc / 8.0) - hpfDb (fs, fc, fc / 16.0), WithinAbs (12.04, 0.05));
            CHECK_THAT (hpfDb (fs, fc, std::min (10.0 * fc, 0.45 * fs)), WithinAbs (0.0, 0.03));
            const auto c = BusToneEq::hpfCoeffs (fs, fc);
            CHECK (std::fpclassify (c.b0 + c.b1 + c.b2) == FP_ZERO);   // a true zero at DC
        }

    SECTION ("the corner clamps to 20 Hz..3 kHz")
    {
        CHECK_THAT (hpfDb (48000.0, 5.0, 20.0), WithinAbs (-3.0103, 0.005));
        CHECK_THAT (hpfDb (48000.0, 9000.0, 3000.0), WithinAbs (-3.0103, 0.005));
    }
}

TEST_CASE ("BusToneEq: a 20 Hz highpass stays clean at high sample rates", "[BusToneEq][bus]")
{
    // Poles this close to z = 1 leave float sections' rounding noise 39 dB
    // under the programme at 192 kHz. Compared against the same coefficients
    // run in long double, the EQ's error has to sit at the float output's own
    // resolution.
    constexpr double fs = 192000.0;
    BusToneEq eq;
    eq.prepare (fs);
    auto t = flat();
    t.hpfOn = true;
    t.hpfHz = 20.0f;

    std::vector<float> in (192000);
    unsigned seed = 3;
    for (size_t i = 0; i < in.size(); ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        in[i] = (float) (0.25 * std::sin (2.0 * kPi * 997.0 * (double) i / fs)
                         + 0.05 * ((double) (seed >> 8) / 16777216.0 - 0.5));
    }
    const auto out = run (eq, in, t, t, 0);

    const auto c = BusToneEq::hpfCoeffs (fs, 20.0);
    long double z1 = 0.0L, z2 = 0.0L;
    double errorSq = 0.0, signalSq = 0.0;
    for (size_t i = 0; i < in.size(); ++i)
    {
        const long double x = in[i];
        const long double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        if (i >= in.size() / 4)
        {
            errorSq += (double) ((out[i] - y) * (out[i] - y));
            signalSq += (double) (y * y);
        }
    }
    CHECK (10.0 * std::log10 (signalSq / errorSq) > 130.0);
}

TEST_CASE ("BusToneEq: a flat EQ is bit-transparent", "[BusToneEq][bus]")
{
    std::vector<float> noise (48000);
    unsigned seed = 1;
    for (auto& v : noise)
    {
        seed = seed * 1664525u + 1013904223u;
        v = (float) (seed >> 8) / 16777216.0f - 0.5f;
    }

    SECTION ("engaged with every band at 0 dB and the highpass off")
    {
        BusToneEq eq;
        eq.prepare (48000.0);
        const auto out = run (eq, noise, flat(), flat(), 0);
        CHECK (std::memcmp (out.data(), noise.data(), noise.size() * sizeof (float)) == 0);
        CHECK (eq.isIdle());
    }

    SECTION ("again once a move back to flat has settled")
    {
        BusToneEq eq;
        eq.prepare (48000.0);
        auto shaped = flat();
        shaped.gainDb = { 9.0f, -9.0f, 9.0f };
        shaped.hpfOn = true;
        shaped.hpfHz = 500.0f;
        const auto out = run (eq, noise, shaped, flat(), 12000);
        // 20 ms glide + 20 ms settling tail, then out of the path.
        const int settled = 12000 + (int) (0.045 * 48000.0);
        CHECK (std::memcmp (out.data() + settled, noise.data() + settled,
                            (noise.size() - (size_t) settled) * sizeof (float)) == 0);
        CHECK (eq.isIdle());
    }
}

TEST_CASE ("BusToneEq: silence in gives silence out", "[BusToneEq][bus]")
{
    BusToneEq eq;
    eq.prepare (48000.0);
    auto shaped = flat();
    shaped.gainDb = { 9.0f, -9.0f, 9.0f };
    shaped.hpfOn = true;
    shaped.hpfHz = 30.0f;
    const auto out = run (eq, std::vector<float> (24000, 0.0f), shaped, shaped, 0);
    CHECK (std::all_of (out.begin(), out.end(), [] (float v) { return std::fpclassify (v) == FP_ZERO; }));
}

TEST_CASE ("BusToneEq: gain, corner and switch moves do not click", "[BusToneEq][bus]")
{
    constexpr double fs = 48000.0;
    // The move starts on a crest of the tone, the worst place to step.
    constexpr int at = 24000 + 12;
    const auto in = sine (fs, 1000.0, 0.25f, 48000);

    struct Move { const char* name; BusToneEq::Targets from, to; double ceilingDb; };
    auto cut = flat();          cut.gainDb = { -9.0f, -9.0f, -9.0f };
    auto boost = flat();        boost.gainDb = { 9.0f, 9.0f, 9.0f };
    auto boostOff = boost;      boostOff.eqOn = false;
    auto hpfLow = flat();       hpfLow.hpfOn = true;  hpfLow.hpfHz = 20.0f;
    auto hpfHigh = hpfLow;      hpfHigh.hpfHz = 3000.0f;
    auto hpfOff = hpfHigh;      hpfOff.hpfOn = false;
    // Stepping the coefficients once per 16 samples instead of ramping them
    // puts the full-range gain move at -35 dBFS; a hard switch at -20.
    const Move moves[] {
        { "bands -9 -> +9 dB", cut, boost, -55.0 },
        { "EQ switched out at +9 dB", boost, boostOff, -55.0 },
        { "highpass 20 Hz -> 3 kHz", hpfLow, hpfHigh, -70.0 },
        { "highpass switched in at 3 kHz", hpfOff, hpfHigh, -70.0 },
    };
    for (const auto& move : moves)
    {
        BusToneEq eq;
        eq.prepare (fs);
        const auto out = run (eq, in, move.from, move.to, at);
        CAPTURE (move.name);
        CHECK (clickPeakDb (out, fs, at, at + 4800) < move.ceilingDb);
    }
}

TEST_CASE ("BusToneEq: out-of-range and non-finite settings are clamped", "[BusToneEq][bus]")
{
    BusToneEq eq;
    eq.prepare (48000.0);
    BusToneEq::Targets wild = flat();
    wild.gainDb = { std::nanf (""), 40.0f, -INFINITY };
    wild.hpfOn = true;
    wild.hpfHz = 1.0e9f;
    const auto in = sine (48000.0, 800.0, 0.1f, 48000);
    const auto out = run (eq, in, wild, wild, 0);
    CHECK (std::all_of (out.begin(), out.end(), [] (float v) { return std::isfinite (v); }));

    // MID clamps to +9 dB and the corner to 3 kHz, so 800 Hz is +9 - 22.8 dB.
    const double expected = 9.0 + hpfDb (48000.0, 3000.0, 800.0);
    CHECK_THAT (rmsDb (out, 24000, 48000) - rmsDb (in, 24000, 48000), WithinAbs (expected, 0.05));
}

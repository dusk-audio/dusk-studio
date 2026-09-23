#include "BusToneEq.h"
#include "../foundation/ScopedNoDenormals.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace duskstudio
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kRampSeconds = 0.020;

using Coeffs = BusToneEq::Coeffs;

// Impulse-invariant poles of s^2 + (w/Q) s + w^2, w in rad/sample, capped
// below Nyquist where the pair would alias onto itself.
void matchedPoles (double w, double Q, double& a1, double& a2) noexcept
{
    w = std::min (w, 0.9 * kPi);
    const double zeta = 0.5 / Q;
    a2 = std::exp (-2.0 * zeta * w);
    if (zeta <= 1.0)
    {
        a1 = -2.0 * std::exp (-zeta * w) * std::cos (std::sqrt (1.0 - zeta * zeta) * w);
    }
    else
    {
        const double s = std::sqrt (zeta * zeta - 1.0);
        a1 = -(std::exp (-w / (zeta + s)) + std::exp (-(zeta + s) * w));
    }
}

// The match point: the band's own frequency while it sits in the lower half
// band, pulled in below it higher up and never above 0.6 pi.
double matchOmega (double w0) noexcept
{
    const double w = w0 <= 0.5 * kPi ? w0 : std::max (0.5 * kPi, 0.7 * w0);
    return std::min (w, 0.6 * kPi);
}

// Numerator whose |B/A|^2 is h0sq at DC, hpisq at Nyquist and hmsq at wm
// (Vicanek, "Matched Second Order Digital Filters", 2016, section 3).
Coeffs matchedNumerator (double a1, double a2, double h0sq, double hpisq,
                         double wm, double hmsq) noexcept
{
    const double A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2);
    const double A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2);
    const double A2 = -4.0 * a2;
    const double sm = std::sin (0.5 * wm);
    const double phi1 = sm * sm;
    const double phi0 = 1.0 - phi1;
    const double phi2 = 4.0 * phi0 * phi1;
    const double B0 = A0 * h0sq;
    const double B1 = A1 * hpisq;
    const double B2 = (hmsq * (A0 * phi0 + A1 * phi1 + A2 * phi2) - B0 * phi0 - B1 * phi1) / phi2;
    const double sB0 = std::sqrt (B0), sB1 = std::sqrt (B1);
    const double W = 0.5 * (sB0 + sB1);
    double b0 = 0.5 * (W + std::sqrt (std::max (0.0, W * W + B2)));
    const double b1 = 0.5 * (sB0 - sB1);
    double b2 = b0 > 0.0 ? -B2 / (4.0 * b0) : 0.0;
    // No real solution leaves the zero pair outside the unit circle; reversing
    // the numerator reflects it inside with the same magnitude, so the cut
    // (the reciprocal) stays stable.
    if (b2 > b0)
        std::swap (b0, b2);
    return { b0, b1, b2, a1, a2 };
}

Coeffs reciprocal (const Coeffs& c) noexcept
{
    const double inv = 1.0 / c.b0;
    return { inv, c.a1 * inv, c.a2 * inv, c.b1 * inv, c.b2 * inv };
}

// Analog |H(jx)|^2 of the RBJ cookbook prototypes, x = w / w0, A = 10^(dB/40).
double peakSq (double x, double A, double Q) noexcept
{
    const double u = (1.0 - x * x) * (1.0 - x * x);
    const double num = x * A / Q, den = x / (A * Q);
    return (u + num * num) / (u + den * den);
}

double shelfSq (double x, double A, double Q, bool high) noexcept
{
    const double x2 = x * x;
    const double mid = (A / (Q * Q)) * x2;
    const double hi = (1.0 - A * x2) * (1.0 - A * x2);
    const double lo = (A - x2) * (A - x2);
    return high ? A * A * (hi + mid) / (lo + mid) : A * A * (lo + mid) / (hi + mid);
}

Coeffs matchedPeak (double fs, double hz, double gainDb, double Q) noexcept
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * hz / fs;
    const double wm = matchOmega (w0);
    double a1, a2;
    matchedPoles (w0, Q * A, a1, a2);
    return matchedNumerator (a1, a2, 1.0, peakSq (kPi / w0, A, Q), wm, peakSq (wm / w0, A, Q));
}

Coeffs matchedShelf (double fs, double hz, double gainDb, double Q, bool high) noexcept
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double sqrtA = std::sqrt (A);
    const double w0 = 2.0 * kPi * hz / fs;
    // A high shelf is matched where its rise begins (its zero frequency).
    const double wm = matchOmega (high ? w0 / sqrtA : w0);
    double a1, a2;
    matchedPoles (high ? w0 * sqrtA : w0 / sqrtA, Q, a1, a2);
    return matchedNumerator (a1, a2, shelfSq (0.0, A, Q, high), shelfSq (kPi / w0, A, Q, high),
                             wm, shelfSq (wm / w0, A, Q, high));
}

float clampGain (float db) noexcept
{
    return std::isfinite (db) ? std::clamp (db, -BusToneEq::kMaxGainDb, BusToneEq::kMaxGainDb) : 0.0f;
}

float clampHpfHz (float hz) noexcept
{
    return std::isfinite (hz) ? std::clamp (hz, BusToneEq::kHpfMinHz, BusToneEq::kHpfMaxHz)
                              : BusToneEq::kHpfMinHz;
}

inline double tdf2 (double x, const Coeffs& c, double& z1, double& z2) noexcept
{
    const double y = c.b0 * x + z1;
    z1 = c.b1 * x - c.a1 * y + z2;
    z2 = c.b2 * x - c.a2 * y;
    return y;
}

inline void addStep (Coeffs& c, const Coeffs& d) noexcept
{
    c.b0 += d.b0; c.b1 += d.b1; c.b2 += d.b2; c.a1 += d.a1; c.a2 += d.a2;
}
} // namespace

BusToneEq::Coeffs BusToneEq::bandCoeffs (Band band, double fs, double gainDb) noexcept
{
    const double hz = band == Low ? kLowHz : band == Mid ? kMidHz : kHighHz;
    const double Q  = band == Mid ? kMidQ : kShelfQ;
    if (! std::isfinite (gainDb) || ! (std::abs (gainDb) > 0.0))
    {
        // Flat: numerator == denominator over the poles the family glides
        // through, so the band is exactly unity and its state stays coherent.
        double a1, a2;
        matchedPoles (2.0 * kPi * hz / fs, Q, a1, a2);
        return { 1.0, a1, a2, a1, a2 };
    }
    const double boost = std::abs (gainDb);
    const Coeffs c = band == Mid ? matchedPeak (fs, hz, boost, Q)
                                 : matchedShelf (fs, hz, boost, Q, band == High);
    return gainDb < 0.0 ? reciprocal (c) : c;
}

BusToneEq::Coeffs BusToneEq::hpfCoeffs (double fs, double cornerHz) noexcept
{
    // Matched highpass (Vicanek 2016): double zero at DC, impulse-invariant
    // poles, gain matched at the pole frequency, so the corner is exactly
    // -3 dB and the passband keeps the analog shape. Bilinear bends the
    // passband up to 0.27 dB flat of analog for a 3 kHz corner at 44.1 kHz.
    const double hz = std::clamp (std::isfinite (cornerHz) ? cornerHz : (double) kHpfMinHz,
                                  (double) kHpfMinHz, std::min ((double) kHpfMaxHz, 0.45 * fs));
    const double w0 = 2.0 * kPi * hz / fs;
    double a1, a2;
    matchedPoles (w0, kHpfQ, a1, a2);
    const double A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2);
    const double A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2);
    const double A2 = -4.0 * a2;
    const double s = std::sin (0.5 * w0);
    const double phi1 = s * s, phi0 = 1.0 - phi1, phi2 = 4.0 * phi0 * phi1;
    const double b0 = kHpfQ * std::sqrt (A0 * phi0 + A1 * phi1 + A2 * phi2) / (4.0 * phi1);
    return { b0, -2.0 * b0, b0, a1, a2 };
}

double BusToneEq::magnitudeDb (const Coeffs& c, double fs, double hz) noexcept
{
    const double w = 2.0 * kPi * hz / fs;
    const double cw = std::cos (w), sw = std::sin (w), c2w = std::cos (2.0 * w), s2w = std::sin (2.0 * w);
    const double nr = c.b0 + c.b1 * cw + c.b2 * c2w, ni = -(c.b1 * sw + c.b2 * s2w);
    const double dr = 1.0 + c.a1 * cw + c.a2 * c2w,  di = -(c.a1 * sw + c.a2 * s2w);
    const double num = nr * nr + ni * ni, den = dr * dr + di * di;
    return 10.0 * std::log10 (std::max (num, 1.0e-300) / std::max (den, 1.0e-300));
}

void BusToneEq::Section::rampTo (const Coeffs& k, int numSamples) noexcept
{
    target = k;
    const double inv = 1.0 / (double) numSamples;
    step = { (k.b0 - c.b0) * inv, (k.b1 - c.b1) * inv, (k.b2 - c.b2) * inv,
             (k.a1 - c.a1) * inv, (k.a2 - c.a2) * inv };
    stepsLeft = numSamples;
}

void BusToneEq::Section::run (float* L, float* R, int numSamples) noexcept
{
    int i = 0;
    if (stepsLeft > 0)
    {
        const int n = std::min (stepsLeft, numSamples);
        for (; i < n; ++i)
        {
            addStep (c, step);
            L[i] = (float) tdf2 (L[i], c, l1, l2);
            R[i] = (float) tdf2 (R[i], c, r1, r2);
        }
        stepsLeft -= n;
        if (stepsLeft == 0)
            c = target;
    }
    const Coeffs k = c;
    for (; i < numSamples; ++i)
    {
        L[i] = (float) tdf2 (L[i], k, l1, l2);
        R[i] = (float) tdf2 (R[i], k, r1, r2);
    }
}

void BusToneEq::Section::runMixed (float* L, float* R, int numSamples,
                                   dusk::audio::SmoothedValue<float>& mix) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        if (stepsLeft > 0)
        {
            addStep (c, step);
            if (--stepsLeft == 0)
                c = target;
        }
        const double w  = mix.getNextValue();
        const double xl = L[i], xr = R[i];
        L[i] = (float) (xl + w * (tdf2 (xl, c, l1, l2) - xl));
        R[i] = (float) (xr + w * (tdf2 (xr, c, r1, r2) - xr));
    }
}

void BusToneEq::prepare (double newSampleRate) noexcept
{
    sampleRate = (newSampleRate > 0.0 && std::isfinite (newSampleRate)) ? newSampleRate : 48000.0;
    // A settled band keeps running this long at unity so the transient of its
    // last move has decayed (past -200 dB for the slowest, LO) before it drops out.
    tailSamples = (int) std::ceil (kRampSeconds * sampleRate);
    for (auto& g : gainDb)
        g.reset (sampleRate, kRampSeconds);
    hpfOctaves.reset (sampleRate, kRampSeconds);
    hpfMix.reset (sampleRate, kRampSeconds);
    reset();
}

void BusToneEq::reset() noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        gainDb[(size_t) b].setCurrentAndTargetValue (gainDb[(size_t) b].getTargetValue());
        auto& s = bands[(size_t) b];
        s.clear();
        s.active = false;
        s.stepsLeft = 0;
    }
    hpfOctaves.setCurrentAndTargetValue (hpfOctaves.getTargetValue());
    hpfMix.setCurrentAndTargetValue (hpfMix.getTargetValue());
    hpf.clear();
    hpf.active = false;
    hpf.stepsLeft = 0;
}

void BusToneEq::setTargets (const Targets& t) noexcept
{
    for (int b = 0; b < kNumBands; ++b)
        gainDb[(size_t) b].setTargetValue (t.eqOn ? clampGain (t.gainDb[(size_t) b]) : 0.0f);

    const float octaves = std::log2 (clampHpfHz (t.hpfHz));
    // Out of the path there is nothing to glide from: the corner snaps.
    if (hpf.active) hpfOctaves.setTargetValue (octaves);
    else            hpfOctaves.setCurrentAndTargetValue (octaves);
    hpfMix.setTargetValue (t.hpfOn ? 1.0f : 0.0f);
}

bool BusToneEq::isIdle() const noexcept
{
    for (int b = 0; b < kNumBands; ++b)
        if (bands[(size_t) b].active || std::abs (gainDb[(size_t) b].getTargetValue()) > 0.0f)
            return false;
    return ! hpf.active && hpfMix.getTargetValue() <= 0.0f;
}

bool BusToneEq::anyGliding() const noexcept
{
    for (const auto& g : gainDb)
        if (g.isSmoothing()) return true;
    return hpf.active && hpfOctaves.isSmoothing();
}

void BusToneEq::updateBand (int band, int numSamples) noexcept
{
    auto& s = bands[(size_t) band];
    auto& g = gainDb[(size_t) band];
    const bool gliding = g.isSmoothing();
    const float value = gliding ? g.skip (numSamples) : g.getTargetValue();

    if (std::abs (value) > 0.0f || gliding)
    {
        if (! s.active)
        {
            // Enter at unity with the family's poles and zero state, so a
            // glide out of flat starts without a step.
            s.clear();
            s.active = true;
            s.jumpTo (bandCoeffs ((Band) band, sampleRate, 0.0));
            s.needsDesign = true;
        }
        s.tailRemaining = tailSamples;
    }
    else if (s.active)
    {
        if (s.tailRemaining <= 0)
        {
            s.active = false;
            s.clear();
            return;
        }
        s.tailRemaining -= numSamples;
    }

    // A settled band keeps the coefficients its glide ended on.
    if (s.active && (gliding || s.needsDesign))
    {
        const Coeffs k = bandCoeffs ((Band) band, sampleRate, value);
        if (gliding) s.rampTo (k, numSamples);
        else         s.jumpTo (k);
        s.needsDesign = false;
    }
}

void BusToneEq::updateHpf (int numSamples) noexcept
{
    const bool wantOn = hpfMix.getTargetValue() > 0.0f;
    if (! hpf.active)
    {
        if (! wantOn) return;
        // The crossfade from dry covers the entry, so the corner starts where
        // it is set.
        hpf.clear();
        hpf.active = true;
        hpf.jumpTo (hpfCoeffs (sampleRate, std::exp2 ((double) hpfOctaves.getTargetValue())));
        return;
    }
    if (! wantOn && ! hpfMix.isSmoothing())
    {
        hpf.active = false;
        hpf.clear();
        return;
    }

    if (hpfOctaves.isSmoothing())
    {
        const float octaves = hpfOctaves.skip (numSamples);
        hpf.rampTo (hpfCoeffs (sampleRate, std::exp2 ((double) octaves)), numSamples);
    }
}

void BusToneEq::process (float* L, float* R, int numSamples) noexcept
{
    dusk::audio::ScopedNoDenormals noDenormals;
    if (numSamples <= 0 || isIdle()) return;

    int offset = 0;
    while (offset < numSamples)
    {
        const int n = anyGliding() ? std::min (kUpdateInterval, numSamples - offset)
                                   : numSamples - offset;
        for (int b = 0; b < kNumBands; ++b)
            updateBand (b, n);
        updateHpf (n);

        float* l = L + offset;
        float* r = R + offset;
        if (hpf.active)
        {
            if (hpfMix.isSmoothing() || hpfMix.getTargetValue() < 1.0f)
                hpf.runMixed (l, r, n, hpfMix);
            else
                hpf.run (l, r, n);
        }
        for (auto& s : bands)
            if (s.active)
                s.run (l, r, n);
        offset += n;
    }
}
} // namespace duskstudio

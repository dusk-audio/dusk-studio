#include "MasteringDigitalEq.h"
#include "../foundation/Decibels.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace duskstudio
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

// JUCE approximatelyEqual, finite path (band freq/gain/Q are always finite):
// relative epsilon, or an absolute floor at the smallest normal.
bool approximatelyEqual (float a, float b) noexcept
{
    const float diff = std::abs (a - b);
    return diff <= std::numeric_limits<float>::min()
        || diff <= std::numeric_limits<float>::epsilon() * std::max (std::abs (a), std::abs (b));
}
} // namespace

void MasteringDigitalEq::prepare (double sampleRate, int /*blockSize*/)
{
    // The biquads run oversampled, so their coefficients are computed at the
    // oversampled rate. Clamp the base rate first: a 0 or non-finite sampleRate
    // from a bad device-rate transition would otherwise poison sr — mirrors the
    // BounceEngine fallback.
    const double baseSr = (sampleRate > 0.0 && std::isfinite (sampleRate)) ? sampleRate : 48000.0;
    sr = baseSr * (double) kOversample;

    // Mastering-friendly defaults - symmetric across the spectrum, every
    // band starts at unity gain so engaging the EQ doesn't change tone.
    static const float defaultFreqs[kNumBands] = { 80.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f };
    static const float defaultQs   [kNumBands] = { 0.7f,  1.0f,    1.0f,   1.0f,    0.7f };

    for (int i = 0; i < kNumBands; ++i)
    {
        bands[(size_t) i].freq.store   (defaultFreqs[i]);
        bands[(size_t) i].gainDb.store (0.0f);
        bands[(size_t) i].q.store      (defaultQs[i]);
        bands[(size_t) i].dirty.store  (true);
    }

    // Halfband-FIR oversampler, one per channel (the functor form is
    // single-channel). Latency is a fixed group delay in base-rate samples;
    // round to the nearest whole sample for integer PDC.
    osL.setFactor (kOversample);
    osR.setFactor (kOversample);
    latencySamples = (int) std::lround (osL.latency());

    reset();
}

void MasteringDigitalEq::reset()
{
    for (auto& f : filtersL) f.reset();
    for (auto& f : filtersR) f.reset();
    osL.reset();
    osR.reset();
}

void MasteringDigitalEq::setBandFreq (int idx, float hz) noexcept
{
    if (idx < 0 || idx >= kNumBands) return;
    auto& b = bands[(size_t) idx];
    if (approximatelyEqual (b.freq.load (std::memory_order_relaxed), hz)) return;
    b.freq.store (hz, std::memory_order_relaxed);
    // Release so the reader (rebuildIfDirty, acquire) sees the new value once
    // it observes dirty — dirty is the synchronisation point for the update.
    b.dirty.store (true, std::memory_order_release);
}

void MasteringDigitalEq::setBandGainDb (int idx, float dB) noexcept
{
    if (idx < 0 || idx >= kNumBands) return;
    auto& b = bands[(size_t) idx];
    if (approximatelyEqual (b.gainDb.load (std::memory_order_relaxed), dB)) return;
    b.gainDb.store (dB, std::memory_order_relaxed);
    b.dirty.store (true, std::memory_order_release);
}

void MasteringDigitalEq::setBandQ (int idx, float q) noexcept
{
    if (idx < 0 || idx >= kNumBands) return;
    auto& b = bands[(size_t) idx];
    if (approximatelyEqual (b.q.load (std::memory_order_relaxed), q)) return;
    b.q.store (q, std::memory_order_relaxed);
    b.dirty.store (true, std::memory_order_release);
}

float MasteringDigitalEq::getBandFreq (int idx) const noexcept
{
    if (idx < 0 || idx >= kNumBands) return 0.0f;
    return bands[(size_t) idx].freq.load (std::memory_order_relaxed);
}

float MasteringDigitalEq::getBandGainDb (int idx) const noexcept
{
    if (idx < 0 || idx >= kNumBands) return 0.0f;
    return bands[(size_t) idx].gainDb.load (std::memory_order_relaxed);
}

float MasteringDigitalEq::getBandQ (int idx) const noexcept
{
    if (idx < 0 || idx >= kNumBands) return 0.0f;
    return bands[(size_t) idx].q.load (std::memory_order_relaxed);
}

void MasteringDigitalEq::writeCoeffs (Filter& f, const Coeffs& c) noexcept
{
    f.setCoeffs ({ c.b0, c.b1, c.b2, c.a1, c.a2 });
}

MasteringDigitalEq::Coeffs MasteringDigitalEq::computeCoeffs (int idx, double sampleRate,
                                                             float freq, float q, float gainDb) noexcept
{
    Coeffs out;
    // Guard every input up front: clamping freq alone still lets a NaN/inf q or
    // gainDb flow into std::pow / std::sin / std::sqrt and produce NaN taps that
    // would blow up the filter. Non-finite anything → passthrough.
    if (sampleRate <= 0.0
        || ! std::isfinite (sampleRate)
        || ! std::isfinite (freq)
        || ! std::isfinite (q)
        || ! std::isfinite (gainDb))
        return out;

    const double f0    = std::clamp ((double) freq, 10.0, sampleRate * 0.49);
    const double w0    = kTwoPi * f0 / sampleRate;
    const double cosw0 = std::cos (w0);
    const double sinw0 = std::sin (w0);
    const double Q     = std::max (0.1, (double) q);
    const double alpha = sinw0 / (2.0 * Q);
    const double A     = std::pow (10.0, (double) gainDb / 40.0);

    // RBJ Audio EQ Cookbook. Single source of truth shared with the UI curve.
    double b0, b1, b2, a0, a1, a2;
    switch (bandType (idx))
    {
        case BandType::LowShelf:
        {
            const double tsa = 2.0 * std::sqrt (A) * alpha;
            b0 =      A * ((A + 1.0) - (A - 1.0) * cosw0 + tsa);
            b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0);
            b2 =      A * ((A + 1.0) - (A - 1.0) * cosw0 - tsa);
            a0 =          (A + 1.0) + (A - 1.0) * cosw0 + tsa;
            a1 =   -2.0 * ((A - 1.0) + (A + 1.0) * cosw0);
            a2 =          (A + 1.0) + (A - 1.0) * cosw0 - tsa;
            break;
        }
        case BandType::HighShelf:
        {
            const double tsa = 2.0 * std::sqrt (A) * alpha;
            b0 =      A * ((A + 1.0) + (A - 1.0) * cosw0 + tsa);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
            b2 =      A * ((A + 1.0) + (A - 1.0) * cosw0 - tsa);
            a0 =          (A + 1.0) - (A - 1.0) * cosw0 + tsa;
            a1 =    2.0 * ((A - 1.0) - (A + 1.0) * cosw0);
            a2 =          (A + 1.0) - (A - 1.0) * cosw0 - tsa;
            break;
        }
        case BandType::Peak:
        default:
        {
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cosw0;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cosw0;
            a2 = 1.0 - alpha / A;
            break;
        }
    }

    const double inv = (std::abs (a0) > 1.0e-20) ? 1.0 / a0 : 0.0;
    out.b0 = (float) (b0 * inv);
    out.b1 = (float) (b1 * inv);
    out.b2 = (float) (b2 * inv);
    out.a1 = (float) (a1 * inv);
    out.a2 = (float) (a2 * inv);

    // A pathological gainDb (huge A) can still overflow a tap to NaN/Inf even with
    // finite inputs; never hand the live filters non-finite coefficients — fall
    // back to passthrough (the default Coeffs: b0 = 1, rest 0).
    if (! (std::isfinite (out.b0) && std::isfinite (out.b1) && std::isfinite (out.b2)
           && std::isfinite (out.a1) && std::isfinite (out.a2)))
        return Coeffs {};
    return out;
}

float MasteringDigitalEq::magnitudeDb (int idx, double sampleRate,
                                       float freq, float q, float gainDb, double atHz) noexcept
{
    if (std::abs (gainDb) < 0.01f
        || sampleRate <= 0.0
        || ! std::isfinite (sampleRate)
        || ! std::isfinite (atHz))
        return 0.0f;

    const Coeffs c = computeCoeffs (idx, sampleRate, freq, q, gainDb);
    const double w   = kTwoPi * atHz / sampleRate;
    const double cw  = std::cos (w),       sw  = std::sin (w);
    const double c2w = std::cos (2.0 * w), s2w = std::sin (2.0 * w);

    const double br = (double) c.b0 + (double) c.b1 * cw + (double) c.b2 * c2w;
    const double bi = -((double) c.b1 * sw + (double) c.b2 * s2w);
    const double ar = 1.0 + (double) c.a1 * cw + (double) c.a2 * c2w;
    const double ai = -((double) c.a1 * sw + (double) c.a2 * s2w);

    const double num = std::sqrt (br * br + bi * bi);
    const double den = std::sqrt (ar * ar + ai * ai);
    const double mag = (den > 1.0e-20) ? num / den : 1.0;
    return (float) dusk::audio::gainToDecibels (mag, -120.0);
}

void MasteringDigitalEq::rebuildIfDirty (int idx) noexcept
{
    auto& b = bands[(size_t) idx];
    // Atomically read-and-clear: exchange (not load + separate store) so a
    // setter's dirty=true that lands concurrently can't be clobbered by the
    // clear and silently lost. Acquire pairs with the setters' release store, so
    // once we observe dirty the freq/q/gainDb writes that preceded it are
    // visible and the relaxed reads below are safe.
    if (! b.dirty.exchange (false, std::memory_order_acquire)) return;

    const float freq = std::clamp (b.freq.load (std::memory_order_relaxed), 10.0f, (float) (sr * 0.49));
    const float qVal = std::clamp (b.q.load (std::memory_order_relaxed), 0.1f, 10.0f);
    const float dB   = b.gainDb.load (std::memory_order_relaxed);

    // Compute + write in place — no heap allocation on the audio thread.
    const Coeffs c = computeCoeffs (idx, sr, freq, qVal, dB);
    writeCoeffs (filtersL[(size_t) idx], c);
    writeCoeffs (filtersR[(size_t) idx], c);
}

void MasteringDigitalEq::processInPlace (float* L, float* R, int numSamples) noexcept
{
    if (sr <= 0.0 || numSamples <= 0 || L == nullptr || R == nullptr)
        return;

    for (int b = 0; b < kNumBands; ++b)
        rebuildIfDirty (b);

    const bool active = enabled.load (std::memory_order_relaxed);

    // Decide once per block which bands actually process. A skipped band (EQ
    // off, or |gain| ≤ 0.05 dB) is cleared here so re-engaging it starts clean
    // instead of ringing out state frozen while it was bypassed.
    bool bandActive[kNumBands];
    for (int b = 0; b < kNumBands; ++b)
    {
        const float g = bands[(size_t) b].gainDb.load (std::memory_order_relaxed);
        bandActive[b] = active && std::abs (g) >= 0.05f;   // ≤ 0.05 dB is inaudible
        if (! bandActive[b])
        {
            filtersL[(size_t) b].reset();
            filtersR[(size_t) b].reset();
        }
    }

    // Always run the oversampler round-trip so the reported latency is constant
    // whether the EQ is engaged or not (matches the limiter's always-latent
    // bypass — a toggle never shifts the master). The functor runs the active
    // bands' series cascade at the oversampled rate; with no active band it is
    // the identity and the round trip is a pure delay.
    for (int i = 0; i < numSamples; ++i)
    {
        L[i] = osL.processSample (L[i], [this, &bandActive] (float s) noexcept
        {
            for (int b = 0; b < kNumBands; ++b)
                if (bandActive[b]) s = filtersL[(size_t) b].process (s);
            return s;
        });
        R[i] = osR.processSample (R[i], [this, &bandActive] (float s) noexcept
        {
            for (int b = 0; b < kNumBands; ++b)
                if (bandActive[b]) s = filtersR[(size_t) b].process (s);
            return s;
        });
    }
}
} // namespace duskstudio

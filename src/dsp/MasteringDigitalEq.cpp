#include "MasteringDigitalEq.h"

namespace duskstudio
{
void MasteringDigitalEq::prepare (double sampleRate, int blockSize)
{
    // The biquads run oversampled, so their coefficients are computed (and the
    // filters prepared) at the oversampled rate. Clamp the base rate first: a 0 or
    // non-finite sampleRate from a bad device-rate transition would otherwise poison
    // sr (and the oversampler / ProcessSpec) — mirrors the BounceEngine fallback.
    const double baseSr = (sampleRate > 0.0 && std::isfinite (sampleRate)) ? sampleRate : 48000.0;
    sr = baseSr * (double) kOversample;
    const int bs = juce::jmax (1, blockSize);

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

    // Oversampler (stereo). IIR polyphase = low latency, no pre-ringing.
    // 2^stages == kOversample (1 stage = 2x, 2 stages = 4x).
    const size_t osStages = (kOversample >= 4) ? 2 : 1;
    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        2, osStages,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
        true /* max quality */, true /* integer latency */);
    oversampler->initProcessing ((size_t) bs);
    oversampler->reset();
    // Integer-latency mode above makes the group delay a whole number of samples,
    // so the reported value is exact — no rounding needed for PDC.
    latencySamples = (int) oversampler->getLatencyInSamples();

    juce::dsp::ProcessSpec spec { sr, (std::uint32_t) (bs * kOversample), 1 };
    for (auto& f : filtersL) { f.prepare (spec); initCoeffs (f); }
    for (auto& f : filtersR) { f.prepare (spec); initCoeffs (f); }

    reset();
}

void MasteringDigitalEq::reset()
{
    for (auto& f : filtersL) f.reset();
    for (auto& f : filtersR) f.reset();
    if (oversampler != nullptr) oversampler->reset();
}

void MasteringDigitalEq::setBandFreq (int idx, float hz) noexcept
{
    if (idx < 0 || idx >= kNumBands) return;
    auto& b = bands[(size_t) idx];
    if (juce::approximatelyEqual (b.freq.load (std::memory_order_relaxed), hz)) return;
    b.freq.store (hz, std::memory_order_relaxed);
    // Release so the reader (rebuildIfDirty, acquire) sees the new value once
    // it observes dirty — dirty is the synchronisation point for the update.
    b.dirty.store (true, std::memory_order_release);
}

void MasteringDigitalEq::setBandGainDb (int idx, float dB) noexcept
{
    if (idx < 0 || idx >= kNumBands) return;
    auto& b = bands[(size_t) idx];
    if (juce::approximatelyEqual (b.gainDb.load (std::memory_order_relaxed), dB)) return;
    b.gainDb.store (dB, std::memory_order_relaxed);
    b.dirty.store (true, std::memory_order_release);
}

void MasteringDigitalEq::setBandQ (int idx, float q) noexcept
{
    if (idx < 0 || idx >= kNumBands) return;
    auto& b = bands[(size_t) idx];
    if (juce::approximatelyEqual (b.q.load (std::memory_order_relaxed), q)) return;
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

void MasteringDigitalEq::initCoeffs (Filter& f)
{
    // Passthrough biquad (b0=1, a0=1). Allocated once on the message thread so
    // the audio thread only ever overwrites the existing taps in place.
    f.coefficients = new juce::dsp::IIR::Coefficients<float> (
        1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
}

void MasteringDigitalEq::writeCoeffs (Filter& f, const Coeffs& c) noexcept
{
    if (f.coefficients == nullptr) return;
    // JUCE stores a normalized biquad as { b0, b1, b2, a1, a2 }.
    auto* raw = f.coefficients->coefficients.getRawDataPointer();
    raw[0] = c.b0; raw[1] = c.b1; raw[2] = c.b2; raw[3] = c.a1; raw[4] = c.a2;
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

    const double f0    = juce::jlimit (10.0, sampleRate * 0.49, (double) freq);
    const double w0    = juce::MathConstants<double>::twoPi * f0 / sampleRate;
    const double cosw0 = std::cos (w0);
    const double sinw0 = std::sin (w0);
    const double Q     = juce::jmax (0.1, (double) q);
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
    const double w   = juce::MathConstants<double>::twoPi * atHz / sampleRate;
    const double cw  = std::cos (w),       sw  = std::sin (w);
    const double c2w = std::cos (2.0 * w), s2w = std::sin (2.0 * w);

    const double br = (double) c.b0 + (double) c.b1 * cw + (double) c.b2 * c2w;
    const double bi = -((double) c.b1 * sw + (double) c.b2 * s2w);
    const double ar = 1.0 + (double) c.a1 * cw + (double) c.a2 * c2w;
    const double ai = -((double) c.a1 * sw + (double) c.a2 * s2w);

    const double num = std::sqrt (br * br + bi * bi);
    const double den = std::sqrt (ar * ar + ai * ai);
    const double mag = (den > 1.0e-20) ? num / den : 1.0;
    return (float) juce::Decibels::gainToDecibels (mag, -120.0);
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

    const float freq = juce::jlimit (10.0f, (float) (sr * 0.49), b.freq.load (std::memory_order_relaxed));
    const float qVal = juce::jlimit (0.1f, 10.0f, b.q.load (std::memory_order_relaxed));
    const float dB   = b.gainDb.load (std::memory_order_relaxed);

    // Compute + write in place — no heap allocation on the audio thread.
    const Coeffs c = computeCoeffs (idx, sr, freq, qVal, dB);
    writeCoeffs (filtersL[(size_t) idx], c);
    writeCoeffs (filtersR[(size_t) idx], c);
}

void MasteringDigitalEq::processInPlace (float* L, float* R, int numSamples) noexcept
{
    if (sr <= 0.0 || numSamples <= 0 || L == nullptr || R == nullptr || oversampler == nullptr)
        return;

    for (int b = 0; b < kNumBands; ++b)
        rebuildIfDirty (b);

    const bool active = enabled.load (std::memory_order_relaxed);

    // Always run the oversampler round-trip so the reported latency is constant
    // whether the EQ is engaged or not (matches the limiter's always-latent
    // bypass — a toggle never shifts the master). Bands apply only when active.
    float* io[2] = { L, R };
    juce::dsp::AudioBlock<float> block (io, 2, (size_t) numSamples);
    auto up = oversampler->processSamplesUp (block);

    if (active)
    {
        const int   upN = (int) up.getNumSamples();
        float* const uL = up.getChannelPointer (0);
        float* const uR = up.getChannelPointer (1);
        for (int b = 0; b < kNumBands; ++b)
        {
            const float g = bands[(size_t) b].gainDb.load (std::memory_order_relaxed);
            if (std::abs (g) < 0.05f)   // ≤ 0.05 dB is inaudible
            {
                // Skipped this block: clear the band's filter so re-engaging it
                // (gain automated back up) starts clean instead of ringing out
                // the stale state frozen while it was bypassed.
                filtersL[(size_t) b].reset();
                filtersR[(size_t) b].reset();
                continue;
            }

            for (int i = 0; i < upN; ++i) uL[i] = filtersL[(size_t) b].processSample (uL[i]);
            for (int i = 0; i < upN; ++i) uR[i] = filtersR[(size_t) b].processSample (uR[i]);
        }
    }
    else
    {
        // EQ disengaged: keep every band's filter clear so toggling it back on
        // doesn't leak the pre-bypass tail.
        for (int b = 0; b < kNumBands; ++b)
        {
            filtersL[(size_t) b].reset();
            filtersR[(size_t) b].reset();
        }
    }

    oversampler->processSamplesDown (block);
}
} // namespace duskstudio

#include "MasteringDigitalEq.h"

namespace duskstudio
{
void MasteringDigitalEq::prepare (double sampleRate, int blockSize)
{
    sr = sampleRate;

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

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) juce::jmax (1, blockSize), 1 };
    for (auto& f : filtersL) f.prepare (spec);
    for (auto& f : filtersR) f.prepare (spec);

    reset();
}

void MasteringDigitalEq::reset()
{
    for (auto& f : filtersL) f.reset();
    for (auto& f : filtersR) f.reset();
}

void MasteringDigitalEq::setBandFreq (int idx, float hz) noexcept
{
    if (idx < 0 || idx >= kNumBands) return;
    bands[(size_t) idx].freq.store (hz, std::memory_order_relaxed);
    bands[(size_t) idx].dirty.store (true, std::memory_order_relaxed);
}

void MasteringDigitalEq::setBandGainDb (int idx, float dB) noexcept
{
    if (idx < 0 || idx >= kNumBands) return;
    bands[(size_t) idx].gainDb.store (dB, std::memory_order_relaxed);
    bands[(size_t) idx].dirty.store (true, std::memory_order_relaxed);
}

void MasteringDigitalEq::setBandQ (int idx, float q) noexcept
{
    if (idx < 0 || idx >= kNumBands) return;
    bands[(size_t) idx].q.store (q, std::memory_order_relaxed);
    bands[(size_t) idx].dirty.store (true, std::memory_order_relaxed);
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

void MasteringDigitalEq::rebuildIfDirty (int idx) noexcept
{
    auto& b = bands[(size_t) idx];
    if (! b.dirty.load (std::memory_order_relaxed)) return;
    b.dirty.store (false, std::memory_order_relaxed);

    const float freq = juce::jlimit (10.0f, (float) (sr * 0.49), b.freq.load());
    const float qVal = juce::jlimit (0.1f, 10.0f, b.q.load());
    const float dB   = b.gainDb.load();
    const float lin  = juce::Decibels::decibelsToGain (dB);

    juce::dsp::IIR::Coefficients<float>::Ptr c;
    switch (bandType (idx))
    {
        case BandType::LowShelf:
            c = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sr, freq, qVal, lin);
            break;
        case BandType::HighShelf:
            c = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, freq, qVal, lin);
            break;
        case BandType::Peak:
        default:
            c = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, freq, qVal, lin);
            break;
    }

    // Filter::coefficients is a Ptr; assigning shares the coefficient
    // pointer - both L and R filters reference the same object so updates
    // never get out of sync.
    filtersL[(size_t) idx].coefficients = c;
    filtersR[(size_t) idx].coefficients = c;
}

void MasteringDigitalEq::processInPlace (float* L, float* R, int numSamples) noexcept
{
    if (sr <= 0.0 || L == nullptr || R == nullptr) return;
    if (! enabled.load (std::memory_order_relaxed)) return;

    for (int b = 0; b < kNumBands; ++b)
        rebuildIfDirty (b);

    // Bypass any band whose gain rounds to 0 dB AND whose coefficient
    // pointer hasn't been built yet. Cheaper than running an inert biquad.
    for (int b = 0; b < kNumBands; ++b)
    {
        const float g = bands[(size_t) b].gainDb.load (std::memory_order_relaxed);
        if (std::abs (g) < 0.05f) continue;  // ≤ 0.05 dB is inaudible

        for (int i = 0; i < numSamples; ++i) L[i] = filtersL[(size_t) b].processSample (L[i]);
        for (int i = 0; i < numSamples; ++i) R[i] = filtersR[(size_t) b].processSample (R[i]);
    }
}
} // namespace duskstudio

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <array>

namespace duskstudio
{
// Five-band digital EQ for the mastering stage.
//
//   Band 0 - Low shelf  (default 80 Hz, Q≈0.7)
//   Band 1 - Low-mid bell
//   Band 2 - Mid bell
//   Band 3 - High-mid bell
//   Band 4 - High shelf (default 12 kHz, Q≈0.7)
//
// Implementation: each band is a juce::dsp::IIR::Filter (minimum-phase
// biquad). They cascade in series per channel. A future iteration can
// replace this with a linear-phase variant for the mastering use case
// (preserves transient timing) - for now the minimum-phase path keeps
// CPU low and matches what most digital console EQs use.
//
// Threading: setBand* / setEnabled run on the message thread; process()
// runs on the audio thread. Coefficients are cached in atomic<float>s
// and re-applied at the start of each block.
class MasteringDigitalEq
{
public:
    static constexpr int kNumBands = 5;

    MasteringDigitalEq() = default;

    void prepare (double sampleRate, int blockSize);
    void reset();

    // Audio thread.
    void processInPlace (float* L, float* R, int numSamples) noexcept;

    // Message thread setters. Each call flags coeffs-dirty for the
    // affected band; the audio thread re-applies before the next block.
    void setEnabled (bool e) noexcept              { enabled.store (e, std::memory_order_relaxed); }
    void setBandFreq    (int idx, float hz) noexcept;
    void setBandGainDb  (int idx, float dB) noexcept;
    void setBandQ       (int idx, float q)  noexcept;

    bool isEnabled() const noexcept                { return enabled.load (std::memory_order_relaxed); }
    float getBandFreq   (int idx) const noexcept;
    float getBandGainDb (int idx) const noexcept;
    float getBandQ      (int idx) const noexcept;

private:
    enum class BandType { LowShelf, Peak, HighShelf };
    static BandType bandType (int idx) noexcept
    {
        if (idx == 0) return BandType::LowShelf;
        if (idx == kNumBands - 1) return BandType::HighShelf;
        return BandType::Peak;
    }

    void rebuildIfDirty (int idx) noexcept;

    double sr = 0.0;
    std::atomic<bool> enabled { false };

    struct Band
    {
        std::atomic<float> freq   { 1000.0f };
        std::atomic<float> gainDb { 0.0f };
        std::atomic<float> q      { 1.0f };
        std::atomic<bool>  dirty  { true };
    };
    std::array<Band, kNumBands> bands;

    // Per channel × per band biquad. JUCE's IIR::Filter is mono - stereo
    // = 2 instances with shared coefficients. We don't share coeffs
    // (each filter owns its state) so we just call setCoefficients on
    // the L and R filter independently.
    using Filter = juce::dsp::IIR::Filter<float>;
    std::array<Filter, kNumBands> filtersL, filtersR;
};
} // namespace duskstudio

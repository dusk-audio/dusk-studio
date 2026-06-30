#pragma once

#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <array>
#include <memory>

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
// biquad), cascaded in series per channel. The cascade runs 2x OVERSAMPLED so
// the shelves/bells stay accurate up to 20 kHz: at the base rate an RBJ high
// shelf/bell craps progressively toward Nyquist (the turnover compresses and
// the gain falls short), which is exactly what a mastering EQ must not do.
// Oversampling pushes Nyquist out of the audio band so the band shapes match
// their analog targets. Adds a few samples of (reported, compensated) latency.
//
// Threading: the param setters are lock-free and allocation-free, so they
// are safe to call from either thread. The chain pulls the latest param
// atomics at block start and the audio thread re-applies any changed band's
// coefficients in place (no heap allocation — see writeCoeffs / computeCoeffs).
class MasteringDigitalEq
{
public:
    static constexpr int kNumBands   = 5;
    static constexpr int kOversample = 4;   // biquads run at this multiple of the base rate

    MasteringDigitalEq() = default;

    void prepare (double sampleRate, int blockSize);
    void reset();

    // Audio thread.
    void processInPlace (float* L, float* R, int numSamples) noexcept;

    // Param setters. Each flags coeffs-dirty for the affected band ONLY when
    // the value actually changes; the audio thread re-applies (in place)
    // before the next block. Lock-free and allocation-free.
    void setEnabled (bool e) noexcept              { enabled.store (e, std::memory_order_relaxed); }
    void setBandFreq    (int idx, float hz) noexcept;
    void setBandGainDb  (int idx, float dB) noexcept;
    void setBandQ       (int idx, float q)  noexcept;

    bool isEnabled() const noexcept                { return enabled.load (std::memory_order_relaxed); }
    float getBandFreq   (int idx) const noexcept;
    float getBandGainDb (int idx) const noexcept;
    float getBandQ      (int idx) const noexcept;

    // Reported in base-rate samples (the 2x oversampler's group delay) so the
    // mastering chain can fold it into its plugin-delay compensation.
    int getLatencySamples() const noexcept { return latencySamples; }

    // Normalized biquad coefficients (a0 == 1). Single source of truth for
    // both the audio path and the UI response curve, so the displayed curve
    // is the actual filter magnitude rather than an approximation.
    struct Coeffs { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; };
    static Coeffs computeCoeffs (int bandIdx, double sampleRate,
                                 float freq, float q, float gainDb) noexcept;
    // Magnitude (dB) of band `bandIdx` at `atHz`. Used by the editor to plot
    // the real filter response. Returns 0 dB for inert (|gain| ~ 0) bands.
    static float magnitudeDb (int bandIdx, double sampleRate,
                              float freq, float q, float gainDb, double atHz) noexcept;

private:
    enum class BandType { LowShelf, Peak, HighShelf };
    static BandType bandType (int idx) noexcept
    {
        if (idx == 0) return BandType::LowShelf;
        if (idx == kNumBands - 1) return BandType::HighShelf;
        return BandType::Peak;
    }

    using Filter = juce::dsp::IIR::Filter<float>;
    // Pre-allocate a passthrough coefficient object (message thread, in
    // prepare) so the audio thread can overwrite its 5 raw taps in place.
    static void initCoeffs  (Filter& f);
    static void writeCoeffs (Filter& f, const Coeffs& c) noexcept;

    void rebuildIfDirty (int idx) noexcept;

    double sr = 0.0;                 // the OVERSAMPLED rate the biquads run at (base * kOversample)
    int    latencySamples = 0;       // oversampler group delay, in base-rate samples
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    std::atomic<bool> enabled { false };

    struct Band
    {
        std::atomic<float> freq   { 1000.0f };
        std::atomic<float> gainDb { 0.0f };
        std::atomic<float> q      { 1.0f };
        std::atomic<bool>  dirty  { true };
    };
    std::array<Band, kNumBands> bands;

    // Per channel × per band biquad. JUCE's IIR::Filter is mono, so L/R are
    // separate instances. Each owns its own coefficient object (pre-allocated
    // in prepare); rebuildIfDirty writes identical taps into both in place.
    std::array<Filter, kNumBands> filtersL, filtersR;
};
} // namespace duskstudio

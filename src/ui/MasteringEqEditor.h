#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <memory>
#include "../session/Session.h"
#include "../dsp/MasteringChain.h"

namespace duskstudio
{
// Custom editor for the mastering chain's MasteringDigitalEq. Shows the
// 5-band frequency-response curve at the top with a row of band controls
// (Freq / Gain / Q) underneath. No dynamic-EQ section - this is a flat
// minimum-phase mastering EQ, period.
//
// Bands:
//   0 - Low shelf
//   1 - Low-mid bell
//   2 - Mid bell
//   3 - High-mid bell
//   4 - High shelf
//
// Shelf bands hide their Q knob (the response-curve evaluator still uses
// Q internally, but the user-facing control isn't useful for shelves at
// the resolution mastering needs).
class MasteringEqEditor final : public juce::Component, private juce::Timer
{
public:
    MasteringEqEditor (MasteringParams& params, MasteringChain* chain);
    ~MasteringEqEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void rebuildKnobValues();

    // Convert plot coords <-> band parameters. Used by both the curve
    // renderer and the drag-handle hit-test / drag-update paths so the
    // visual dot position and the audio path always agree.
    float dbToY      (float db) const noexcept;
    float yToDb      (float y)  const noexcept;
    float freqToX    (float hz) const noexcept;
    float xToFreq    (float x)  const noexcept;
    int   hitTestBandDot (juce::Point<int> p) const noexcept;

    // Per-band response in dB at a given frequency. Uses standard analog
    // peaking / shelving prototype formulas - close enough to the deployed
    // biquad's response that the curve reads as the audible shape, while
    // staying cheap (no biquad eval inside paint()).
    float bandResponseDb (int idx, float freqHz) const noexcept;
    float totalResponseDb (float freqHz) const noexcept;

    // Pull the latest post-EQ window from the chain, window + FFT it, fold
    // the magnitudes into specDb (peak-attack / slow-release). Returns true
    // if specDb changed (→ repaint the curve area).
    bool updateSpectrum();
    void drawSpectrum (juce::Graphics&, juce::Rectangle<float> plot) const;

    MasteringParams& params;
    MasteringChain*  chain = nullptr;

    // ── Spectrum analyzer ──
    static constexpr int kFftOrder = 11;            // 2048-point
    static constexpr int kFftSize  = 1 << kFftOrder;
    static constexpr int kNumBins  = kFftSize / 2;

    bool showSpectrum = true;
    juce::TextButton specToggle { "FFT" };

    juce::dsp::FFT fft { kFftOrder };
    juce::dsp::WindowingFunction<float> fftWindow
        { (size_t) kFftSize, juce::dsp::WindowingFunction<float>::hann, false };
    std::array<float, kFftSize>     fftScratch {};   // [time | imag]; FFT wants 2×size
    std::array<float, kFftSize * 2> fftWork {};
    std::array<float, kNumBins>     specDb;          // smoothed magnitudes, dBFS

    // Shared console chrome: LED-pill header (click toggles enable), same
    // component the channel / bus / master strips use for their EQ + COMP
    // headers — so the mastering panels read as part of the same desk.
    std::unique_ptr<class CompHeaderButton> headerBtn;

    struct BandUI
    {
        juce::Slider freqKnob;
        juce::Slider gainKnob;
        juce::Slider qKnob;        // hidden for shelf bands
        juce::Label  bandLabel;    // "Low" / "Lo-Mid" / "Mid" / "Hi-Mid" / "High"
        juce::Colour accent;
    };
    std::array<BandUI, 5> bandUI;

    juce::Rectangle<int> curveArea;
    juce::Rectangle<int> controlsArea;

    // Drag state. -1 = not dragging. While dragging, mouseDrag updates the
    // band's freq + gain atomics and re-syncs the corresponding knobs so
    // the per-band controls below the curve track the drag in real time.
    int draggingBand = -1;

    // Cached value snapshots so we only repaint the curve when the user
    // actually moves something. The Timer polls and triggers repaint when
    // a band parameter has changed.
    std::array<float, 5> lastFreq {};
    std::array<float, 5> lastGain {};
    std::array<float, 5> lastQ    {};
    bool lastEnabled = false;
};
} // namespace duskstudio

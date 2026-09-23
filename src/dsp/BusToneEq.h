#pragma once

#include "../foundation/SmoothedValue.h"

#include <array>

namespace duskstudio
{
// The bus strip's Tone EQ: a clean digital tone control with no saturation.
//
//   LO   shelf 300 Hz, slope S = 1   +/-9 dB
//   MID  bell  800 Hz, Q 0.7         +/-9 dB
//   HI   shelf 2 kHz,  slope S = 1   +/-9 dB
//   HPF  12 dB/oct Butterworth, corner (-3 dB) 20 Hz..3 kHz
//
// A shelf's frequency is its half-gain point and its gain is its plateau, so a
// +9 dB LO reads +4.5 dB at 300 Hz and +9 dB towards DC. S = 1 is the RBJ
// cookbook's steepest shelf that stays monotonic (Q 1/sqrt2 at every gain).
//
// Every section is a matched-magnitude biquad (Vicanek 2016): impulse-invariant
// poles, numerator solved so |H| equals the analog prototype at DC, Nyquist and
// one in-band point. They keep their analog shape up to Nyquist at the native
// rate, so the EQ never needs oversampling.
//
// The sections run in double. A 20 Hz highpass in float keeps poles so close
// to z = 1 that its rounding noise sits only 58 dB under the programme at
// 44.1 kHz and 39 dB under it at 192 kHz.
//
// Gains and the HPF corner glide over 20 ms (the corner in octaves). While
// anything glides the sections are redesigned every kUpdateInterval samples
// and their coefficients ramp linearly in between, so a move has no steps;
// otherwise nothing is redesigned. A band settled at 0 dB drops out once its
// transient has decayed, and a disengaged HPF crossfades to dry and drops out,
// so a flat EQ is bit-transparent.
class BusToneEq
{
public:
    enum Band { Low = 0, Mid, High, kNumBands };

    static constexpr double kLowHz   = 300.0;
    static constexpr double kMidHz   = 800.0;
    static constexpr double kHighHz  = 2000.0;
    static constexpr double kMidQ    = 0.7;
    static constexpr double kShelfQ  = 0.70710678118654752440;
    static constexpr double kHpfQ    = 0.70710678118654752440;
    static constexpr float  kMaxGainDb = 9.0f;
    static constexpr float  kHpfMinHz  = 20.0f;
    static constexpr float  kHpfMaxHz  = 3000.0f;
    static constexpr int    kUpdateInterval = 16;

    struct Targets
    {
        bool  eqOn = false;
        std::array<float, kNumBands> gainDb {};
        bool  hpfOn = false;
        float hpfHz = kHpfMinHz;
    };

    // a0-normalised.
    struct Coeffs { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    // Audio thread, once per block before process(). Values are clamped here.
    void setTargets (const Targets& t) noexcept;
    void process (float* L, float* R, int numSamples) noexcept;

    // Every section is out of the path: process() leaves the buffers untouched.
    bool isIdle() const noexcept;

    static Coeffs bandCoeffs (Band band, double sampleRate, double gainDb) noexcept;
    static Coeffs hpfCoeffs (double sampleRate, double cornerHz) noexcept;
    static double magnitudeDb (const Coeffs& c, double sampleRate, double hz) noexcept;

private:
    struct Section
    {
        Coeffs c, step, target;
        int    stepsLeft = 0;
        double l1 = 0.0, l2 = 0.0, r1 = 0.0, r2 = 0.0;
        bool   active = false;
        bool   needsDesign = false;
        int    tailRemaining = 0;

        void clear() noexcept { l1 = l2 = r1 = r2 = 0.0; }
        void jumpTo (const Coeffs& k) noexcept { c = k; stepsLeft = 0; }
        void rampTo (const Coeffs& k, int numSamples) noexcept;
        void run (float* L, float* R, int numSamples) noexcept;
        void runMixed (float* L, float* R, int numSamples, dusk::audio::SmoothedValue<float>& mix) noexcept;
    };

    void updateBand (int band, int numSamples) noexcept;
    void updateHpf (int numSamples) noexcept;
    bool anyGliding() const noexcept;

    double sampleRate = 48000.0;
    int    tailSamples = 960;

    std::array<Section, kNumBands> bands;
    std::array<dusk::audio::SmoothedValue<float>, kNumBands> gainDb;

    Section hpf;
    dusk::audio::SmoothedValue<float> hpfOctaves;   // log2 (corner Hz)
    dusk::audio::SmoothedValue<float> hpfMix;       // 0 dry .. 1 filtered
};
} // namespace duskstudio

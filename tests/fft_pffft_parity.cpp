#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "foundation/Fft.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <complex>
#include <random>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
std::vector<float> randomSignal (int n, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    std::vector<float> v ((size_t) n);
    for (auto& s : v) s = dist (rng);
    return v;
}

// Absolute tolerance scaled by the transform size: an N-point FFT of unit-
// amplitude input accumulates N terms, so float round-off grows with N.
float tolFor (int n) { return 1.0e-4f * (float) n / 1024.0f; }
} // namespace

TEST_CASE ("Fft real forward matches the reference layout", "[fft]")
{
    // Order 5 is pffft's minimum and the floor crossCorrelate clamps to.
    const int order = GENERATE (5, 10);
    const int n = 1 << order;

    dusk::audio::Fft dut { order };
    juce::dsp::FFT   ref { order };

    std::vector<float> signal;

    SECTION ("random noise")   { signal = randomSignal (n, 4242); }
    SECTION ("impulse")        { signal.assign ((size_t) n, 0.0f); signal[3] = 1.0f; }
    SECTION ("dc")             { signal.assign ((size_t) n, 0.75f); }
    SECTION ("nyquist rate")
    {
        signal.resize ((size_t) n);
        for (int i = 0; i < n; ++i) signal[(size_t) i] = (i % 2 == 0) ? 1.0f : -1.0f;
    }

    // Sentinel-filled so the zeroed DC / Nyquist imaginary slots below are a
    // real assertion rather than a leftover from the initial fill.
    std::vector<float> a ((size_t) (2 * n), 5.0f), b ((size_t) (2 * n), 5.0f);
    std::copy (signal.begin(), signal.end(), a.begin());
    std::copy (signal.begin(), signal.end(), b.begin());

    dut.performRealOnlyForwardTransform (a.data(), true);
    ref.performRealOnlyForwardTransform (b.data(), true);

    const float tol = tolFor (n);
    for (int k = 0; k <= n / 2; ++k)
    {
        REQUIRE_THAT (a[(size_t) (2 * k)],     WithinAbs (b[(size_t) (2 * k)],     tol));
        REQUIRE_THAT (a[(size_t) (2 * k + 1)], WithinAbs (b[(size_t) (2 * k + 1)], tol));
    }

    REQUIRE_THAT (a[1], WithinAbs (0.0f, 0.0f));
    REQUIRE_THAT (a[(size_t) (n + 1)], WithinAbs (0.0f, 0.0f));
}

TEST_CASE ("Fft real forward mirrors the negative frequencies on request", "[fft]")
{
    constexpr int order = 10;
    constexpr int n = 1 << order;

    dusk::audio::Fft dut { order };
    juce::dsp::FFT   ref { order };

    const auto signal = randomSignal (n, 99);
    std::vector<float> a ((size_t) (2 * n), 0.0f), b ((size_t) (2 * n), 0.0f);
    std::copy (signal.begin(), signal.end(), a.begin());
    std::copy (signal.begin(), signal.end(), b.begin());

    dut.performRealOnlyForwardTransform (a.data(), false);
    ref.performRealOnlyForwardTransform (b.data(), false);

    const float tol = tolFor (n);
    for (int i = 0; i < 2 * n; ++i)
        REQUIRE_THAT (a[(size_t) i], WithinAbs (b[(size_t) i], tol));
}

TEST_CASE ("Fft frequency-only forward matches magnitudes and zeroes the remainder", "[fft]")
{
    constexpr int order = 11;
    constexpr int n = 1 << order;

    dusk::audio::Fft dut { order };
    juce::dsp::FFT   ref { order };

    const auto signal = randomSignal (n, 7);
    std::vector<float> a ((size_t) (2 * n), 1.0f), b ((size_t) (2 * n), 1.0f);
    std::copy (signal.begin(), signal.end(), a.begin());
    std::copy (signal.begin(), signal.end(), b.begin());

    dut.performFrequencyOnlyForwardTransform (a.data());
    ref.performFrequencyOnlyForwardTransform (b.data(), true);

    const float tol = tolFor (n);
    for (int k = 0; k <= n / 2; ++k)
        REQUIRE_THAT (a[(size_t) k], WithinAbs (b[(size_t) k], tol));

    for (int i = n / 2 + 1; i < 2 * n; ++i)
        REQUIRE_THAT (a[(size_t) i], WithinAbs (0.0f, 0.0f));
}

TEST_CASE ("Fft complex forward matches the reference", "[fft]")
{
    constexpr int order = 12;
    constexpr int n = 1 << order;

    dusk::audio::Fft dut { order };
    juce::dsp::FFT   ref { order };

    const auto re = randomSignal (n, 11);
    const auto im = randomSignal (n, 12);

    std::vector<std::complex<float>> in ((size_t) n), outDut ((size_t) n);
    std::vector<juce::dsp::Complex<float>> inRef ((size_t) n), outRef ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        in[(size_t) i]    = { re[(size_t) i], im[(size_t) i] };
        inRef[(size_t) i] = { re[(size_t) i], im[(size_t) i] };
    }

    dut.perform (in.data(), outDut.data(), false);
    ref.perform (inRef.data(), outRef.data(), false);

    const float tol = tolFor (n);
    for (int i = 0; i < n; ++i)
    {
        REQUIRE_THAT (outDut[(size_t) i].real(), WithinAbs (outRef[(size_t) i].real(), tol));
        REQUIRE_THAT (outDut[(size_t) i].imag(), WithinAbs (outRef[(size_t) i].imag(), tol));
    }
}

TEST_CASE ("Fft inverse applies the same 1/N scaling as the reference", "[fft]")
{
    constexpr int order = 12;
    constexpr int n = 1 << order;

    dusk::audio::Fft dut { order };
    juce::dsp::FFT   ref { order };

    const auto re = randomSignal (n, 21);
    const auto im = randomSignal (n, 22);

    std::vector<std::complex<float>> in ((size_t) n), spec ((size_t) n), back ((size_t) n);
    std::vector<juce::dsp::Complex<float>> inRef ((size_t) n), specRef ((size_t) n), backRef ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        in[(size_t) i]    = { re[(size_t) i], im[(size_t) i] };
        inRef[(size_t) i] = { re[(size_t) i], im[(size_t) i] };
    }

    dut.perform (in.data(), spec.data(), false);
    dut.perform (spec.data(), back.data(), true);
    ref.perform (inRef.data(), specRef.data(), false);
    ref.perform (specRef.data(), backRef.data(), true);

    const float tol = tolFor (n);
    for (int i = 0; i < n; ++i)
    {
        // Round trip returns the input unscaled - the inverse carries 1/N.
        REQUIRE_THAT (back[(size_t) i].real(), WithinAbs (re[(size_t) i], tol));
        REQUIRE_THAT (back[(size_t) i].imag(), WithinAbs (im[(size_t) i], tol));
        REQUIRE_THAT (back[(size_t) i].real(), WithinAbs (backRef[(size_t) i].real(), tol));
        REQUIRE_THAT (back[(size_t) i].imag(), WithinAbs (backRef[(size_t) i].imag(), tol));
    }
}

TEST_CASE ("Fft complex transform tolerates an aliased in/out buffer", "[fft]")
{
    constexpr int order = 10;
    constexpr int n = 1 << order;

    dusk::audio::Fft dut { order };

    const auto re = randomSignal (n, 33);
    const auto im = randomSignal (n, 34);

    std::vector<std::complex<float>> in ((size_t) n), out ((size_t) n);
    for (int i = 0; i < n; ++i) in[(size_t) i] = { re[(size_t) i], im[(size_t) i] };

    dut.perform (in.data(), out.data(), false);

    auto inPlace = in;
    dut.perform (inPlace.data(), inPlace.data(), false);

    for (int i = 0; i < n; ++i)
    {
        REQUIRE_THAT (inPlace[(size_t) i].real(), WithinAbs (out[(size_t) i].real(), 0.0f));
        REQUIRE_THAT (inPlace[(size_t) i].imag(), WithinAbs (out[(size_t) i].imag(), 0.0f));
    }
}

TEST_CASE ("Fft Hann table matches the reference window", "[fft]")
{
    constexpr int n = 2048;

    std::vector<float> table ((size_t) n, 0.0f);
    dusk::audio::Fft::fillHannWindow (table.data(), n);

    std::vector<float> refTable ((size_t) n, 1.0f);
    juce::dsp::WindowingFunction<float> window
        { (size_t) n, juce::dsp::WindowingFunction<float>::hann, false };
    window.multiplyWithWindowingTable (refTable.data(), (size_t) n);

    for (int i = 0; i < n; ++i)
        REQUIRE_THAT (table[(size_t) i], WithinAbs (refTable[(size_t) i], 1.0e-6f));
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../src/dsp/PhaseAnalyzer.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using duskstudio::PhaseAnalyzer;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;

std::vector<float> noise (int samples, std::uint32_t seed = 12345)
{
    std::vector<float> values ((size_t) samples);
    for (auto& value : values)
    {
        seed = seed * 1664525u + 1013904223u;
        value = (static_cast<float> (seed >> 8) / 8388608.0f - 1.0f) * 0.1f;
    }
    return values;
}

int nearestBand (const PhaseAnalyzer& analyzer, double frequency)
{
    const auto& report = analyzer.report();
    int best = 0;
    for (int i = 1; i < report.settings.bands; ++i)
        if (std::abs (std::log (report.bands[(size_t) i].centerHz / frequency))
            < std::abs (std::log (report.bands[(size_t) best].centerHz / frequency)))
            best = i;
    return best;
}
}

TEST_CASE ("Phase analyzer measures matching and inverted stereo in four or sixty-four bands", "[mastering][phase]")
{
    for (const int bands : { 4, 64 })
        for (const float polarity : { 1.0f, -1.0f })
        {
            CAPTURE (bands, polarity);
            PhaseAnalyzer analyzer;
            analyzer.prepare (kRate, { bands, 2.0f, 50.0f });
            const auto period = noise (analyzer.windowSize());
            std::vector<float> left (period.size() * 12);
            for (size_t i = 0; i < left.size(); ++i) left[i] = period[i % period.size()];
            auto right = left;
            for (auto& value : right) value *= polarity;
            analyzer.push (left.data(), right.data(), (int) left.size());
            const auto& report = analyzer.report();
            REQUIRE_FALSE (report.warming);
            for (int i = 0; i <= bands; ++i)
            {
                const auto& band = i == bands ? report.broadband : report.bands[(size_t) i];
                REQUIRE (band.correlationValid);
                REQUIRE (band.minimumValid);
                REQUIRE_THAT (band.correlation, WithinAbs (polarity, 1e-6));
                REQUIRE_THAT (band.minimumCorrelation, WithinAbs (polarity, 1e-6));
                REQUIRE (band.coherenceValid);
                REQUIRE_THAT (band.coherence, WithinAbs (1.0, 1e-6));
                REQUIRE (band.monoValid);
                if (polarity > 0.0f)
                    REQUIRE_THAT (band.monoLossDb, WithinAbs (0.0, 1e-6));
                else
                    REQUIRE (band.monoLossDb < -100.0);
            }
        }
}

TEST_CASE ("Phase analyzer separates unrelated stereo noise after statistical settling", "[mastering][phase]")
{
    PhaseAnalyzer analyzer;
    analyzer.prepare (kRate, { 8, 1.0f, 2000.0f });
    const auto left = noise (analyzer.windowSize() * 40);
    const auto right = noise ((int) left.size(), 987654321);
    const std::vector<float> silence (left.size());
    analyzer.push (silence.data(), silence.data(), (int) silence.size());
    const int firstWindow = analyzer.windowSize();
    analyzer.push (left.data(), right.data(), firstWindow);
    REQUIRE_FALSE (analyzer.report().broadband.coherenceValid);
    for (int i = 0; i < 8; ++i)
        REQUIRE_FALSE (analyzer.report().bands[(size_t) i].coherenceValid);
    analyzer.push (left.data() + firstWindow, right.data() + firstWindow, (int) left.size() - firstWindow);
    double coherence = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        const auto& band = analyzer.report().bands[(size_t) i];
        REQUIRE (band.correlationValid);
        REQUIRE (std::abs (band.correlation) < 0.25);
        REQUIRE (band.coherenceValid);
        REQUIRE (band.coherence < 0.45);
        coherence += band.coherence;
    }
    REQUIRE (coherence / 8.0 < 0.2);
    REQUIRE_THAT (analyzer.report().broadband.monoLossDb, WithinAbs (-3.0103, 0.15));
}

TEST_CASE ("Phase analyzer finds a localized inversion hidden by broadband correlation", "[mastering][phase]")
{
    PhaseAnalyzer analyzer;
    analyzer.prepare (kRate, { 64, 8.0f, 50.0f });
    auto left = noise (analyzer.windowSize() * 2);
    auto right = left;
    for (size_t i = 0; i < left.size(); ++i)
    {
        const double t = (double) i / kRate;
        const float common = (float) (0.2 * (std::sin (2 * kPi * 500 * t) + std::sin (2 * kPi * 10000 * t)));
        const float inverted = (float) (0.2 * std::sin (2 * kPi * 3000 * t));
        left[i] = left[i] * 0.01f + common + inverted;
        right[i] = right[i] * 0.01f + common - inverted;
    }
    analyzer.push (left.data(), right.data(), (int) left.size());
    REQUIRE (analyzer.report().broadband.correlation > 0.2);
    REQUIRE (analyzer.report().bands[(size_t) nearestBand (analyzer, 3000)].correlation < -0.95);
    REQUIRE (analyzer.report().bands[(size_t) nearestBand (analyzer, 500)].correlation > 0.95);
    REQUIRE (analyzer.report().bands[(size_t) nearestBand (analyzer, 10000)].correlation > 0.95);
}

TEST_CASE ("Phase analyzer distinguishes broadband delay from single-tone ambiguity", "[mastering][phase]")
{
    PhaseAnalyzer analyzer;
    analyzer.prepare (kRate, { 16, 2.0f, 50.0f });
    constexpr int kDelay = 48;
    const int samples = analyzer.windowSize() * 48;
    auto left = noise (samples);
    std::vector<float> right ((size_t) samples);
    SECTION ("delayed repeating broadband noise stays coherent across phase rotations")
    {
        const auto period = noise (analyzer.windowSize());
        for (int i = 0; i < samples; ++i)
        {
            left[(size_t) i] = period[(size_t) i % period.size()];
            right[(size_t) i] = period[((size_t) i + period.size() - kDelay) % period.size()];
        }
        analyzer.push (left.data(), right.data(), samples);
        for (const double frequency : { 500.0, 3000.0, 10000.0 })
        {
            const auto& band = analyzer.report().bands[(size_t) nearestBand (analyzer, frequency)];
            REQUIRE (band.coherenceValid);
            REQUIRE (band.coherence > 0.95);
            REQUIRE (band.delayValid);
            REQUIRE_THAT (band.delayMs, WithinAbs (1000.0 * kDelay / kRate, 0.02));
        }
    }
    SECTION ("one sinusoid only has a wrapped phase-equivalent offset")
    {
        const double binHz = kRate / analyzer.windowSize();
        const double frequency = std::round (1000.0 / binHz) * binHz;
        constexpr int kToneDelay = 4;
        for (int i = 0; i < samples; ++i)
        {
            left[(size_t) i] = (float) (0.2 * std::sin (2 * kPi * frequency * i / kRate));
            right[(size_t) i] = (float) (0.2 * std::sin (2 * kPi * frequency * (i - kToneDelay) / kRate));
        }
        analyzer.push (left.data(), right.data(), samples);
        const auto& band = analyzer.report().bands[(size_t) nearestBand (analyzer, frequency)];
        REQUIRE (band.coherenceValid);
        REQUIRE (band.coherence > 0.99);
        REQUIRE_FALSE (band.delayValid);
        REQUIRE (band.phaseValid);
        REQUIRE_THAT (band.equivalentDelayMs, WithinAbs (1000.0 * kToneDelay / kRate, 0.002));
    }
    SECTION ("separate coherent spectral islands cannot hide whole phase cycles")
    {
        const double binHz = kRate / analyzer.windowSize();
        const int gapBins = (int) std::round (2000.0 / binHz);
        const int bandIndex = nearestBand (analyzer, 4000.0);
        const double center = analyzer.report().bands[(size_t) bandIndex].centerHz;
        const double gap = gapBins * binHz;
        const int firstBin = (int) std::round ((std::sqrt (gap * gap + 4 * center * center) - gap) * 0.5 / binHz);
        const double delaySeconds = 1.0 / (gapBins * binHz);
        for (int i = 0; i < samples; ++i)
        {
            double l = 0.0, r = 0.0;
            for (const int island : { 0, gapBins })
                for (int offset = -8; offset <= 8; offset += 2)
                {
                    const double frequency = (firstBin + island + offset) * binHz;
                    const double phase = offset * 0.37;
                    l += 0.02 * std::sin (2 * kPi * frequency * i / kRate + phase);
                    r += 0.02 * std::sin (2 * kPi * frequency * (i / kRate - delaySeconds) + phase);
                }
            left[(size_t) i] = (float) l;
            right[(size_t) i] = (float) r;
        }
        analyzer.push (left.data(), right.data(), samples);
        const auto& band = analyzer.report().bands[(size_t) bandIndex];
        REQUIRE (band.coherenceValid);
        REQUIRE (band.coherence > 0.99);
        REQUIRE_FALSE (band.delayValid);
    }
    SECTION ("a newly energized quadrature tone cannot steer a settled phase reading")
    {
        const auto period = noise (analyzer.windowSize());
        for (int i = 0; i < samples; ++i) left[(size_t) i] = period[(size_t) i % period.size()];
        analyzer.push (left.data(), left.data(), samples);
        const int bandIndex = nearestBand (analyzer, 500.0);
        const double binHz = kRate / analyzer.windowSize();
        const double frequency = std::round (analyzer.report().bands[(size_t) bandIndex].centerHz / binHz) * binHz;
        auto nextL = period, nextR = period;
        for (size_t i = 0; i < period.size(); ++i)
        {
            nextL[i] += (float) (0.015 * std::sin (2 * kPi * frequency * i / kRate));
            nextR[i] += (float) (0.015 * std::cos (2 * kPi * frequency * i / kRate));
        }
        analyzer.push (nextL.data(), nextR.data(), (int) nextL.size());
        const auto& band = analyzer.report().bands[(size_t) bandIndex];
        REQUIRE (band.coherenceValid);
        REQUIRE (band.phaseValid);
        REQUIRE_THAT (band.phaseDegrees, WithinAbs (0.0, 0.5));
    }
}

TEST_CASE ("Phase analyzer Q narrows a band's spectral selection", "[mastering][phase]")
{
    PhaseAnalyzer wide, narrow;
    wide.prepare (kRate, { 64, 0.25f, 50.0f });
    narrow.prepare (kRate, { 64, 8.0f, 50.0f });
    const int samples = narrow.windowSize() * 2;
    std::vector<float> left ((size_t) samples), right ((size_t) samples);
    for (int i = 0; i < samples; ++i)
    {
        const double t = (double) i / kRate;
        const float common = (float) (0.2 * std::sin (2 * kPi * 1000 * t));
        const float inverted = (float) (0.2 * std::sin (2 * kPi * 3000 * t));
        left[(size_t) i] = common + inverted;
        right[(size_t) i] = common - inverted;
    }
    wide.push (left.data(), right.data(), samples);
    narrow.push (left.data(), right.data(), samples);
    const auto& narrowBand = narrow.report().bands[(size_t) nearestBand (narrow, 3000)];
    const auto& wideBand = wide.report().bands[(size_t) nearestBand (wide, 3000)];
    REQUIRE (narrowBand.correlationValid);
    REQUIRE (wideBand.correlationValid);
    REQUIRE (narrowBand.correlation < -0.95);
    REQUIRE (wideBand.correlation > -0.4);
}

TEST_CASE ("Phase analyzer correlation averaging follows its time constant after the FFT window", "[mastering][phase]")
{
    PhaseAnalyzer analyzer;
    analyzer.prepare (kRate, { 16, 2.0f, 1000.0f });
    const double binHz = kRate / analyzer.windowSize();
    const double frequency = std::round (1000.0 / binHz) * binHz;
    const int samples = analyzer.windowSize() * 8;
    std::vector<float> left ((size_t) samples), right ((size_t) samples);
    for (int i = 0; i < samples; ++i)
        left[(size_t) i] = right[(size_t) i] = (float) (0.2 * std::sin (2 * kPi * frequency * i / kRate));
    analyzer.push (left.data(), right.data(), samples);
    for (auto& value : right) value = -value;
    analyzer.push (left.data(), right.data(), analyzer.windowSize() * 2);
    const int band = nearestBand (analyzer, frequency);
    const double before = analyzer.report().bands[(size_t) band].correlation;
    const auto framesBefore = analyzer.report().frames;
    analyzer.push (left.data(), right.data(), analyzer.windowSize() * 3);
    const double after = analyzer.report().bands[(size_t) band].correlation;
    const double elapsed = (double) (analyzer.report().frames - framesBefore) * analyzer.hopSize() / kRate;
    REQUIRE_THAT ((after + 1.0) / (before + 1.0), WithinAbs (std::exp (-elapsed), 1e-4));
}

TEST_CASE ("Phase analyzer rejects undefined stereo measurements without inventing warnings", "[mastering][phase]")
{
    PhaseAnalyzer analyzer;
    analyzer.prepare (kRate, { 16, 2.0f, 50.0f });
    const auto left = noise (analyzer.windowSize() * 12);
    std::vector<float> silence (left.size());
    analyzer.push (silence.data(), silence.data(), (int) silence.size());
    REQUIRE_FALSE (analyzer.report().broadband.monoValid);
    REQUIRE_FALSE (analyzer.report().broadband.correlationValid);
    REQUIRE_FALSE (analyzer.report().broadband.minimumValid);
    REQUIRE_FALSE (analyzer.report().broadband.flagged);
    analyzer.push (left.data(), silence.data(), (int) left.size());
    const auto& report = analyzer.report();
    REQUIRE (report.broadband.monoValid);
    REQUIRE_THAT (report.broadband.monoLossDb, WithinAbs (-3.0103, 1e-4));
    REQUIRE_FALSE (report.broadband.correlationValid);
    REQUIRE_FALSE (report.broadband.coherenceValid);
    REQUIRE_FALSE (report.broadband.delayValid);
    REQUIRE_FALSE (report.broadband.minimumValid);
    REQUIRE_FALSE (report.broadband.flagged);
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    analyzer.push (&invalid, &invalid, 1);
    REQUIRE (analyzer.report().warming);
    REQUIRE_FALSE (analyzer.report().broadband.monoValid);
}

TEST_CASE ("Phase analyzer retains held warnings across discontinuities until Reset", "[mastering][phase]")
{
    PhaseAnalyzer analyzer;
    analyzer.prepare (kRate, { 16, 2.0f, 50.0f });
    const auto left = noise (analyzer.windowSize() * 4);
    auto right = left;
    for (auto& value : right) value = -value;
    analyzer.push (left.data(), right.data(), (int) left.size());
    REQUIRE (analyzer.report().broadband.flagged);
    analyzer.markDiscontinuity();
    REQUIRE (analyzer.report().warming);
    REQUIRE_FALSE (analyzer.report().broadband.correlationValid);
    REQUIRE (analyzer.report().broadband.flagged);
    analyzer.push (left.data(), left.data(), analyzer.windowSize() - 1);
    REQUIRE (analyzer.report().warming);
    analyzer.push (left.data(), left.data(), 1);
    REQUIRE_THAT (analyzer.report().broadband.correlation, WithinAbs (1.0, 1e-6));
    REQUIRE_THAT (analyzer.report().broadband.minimumCorrelation, WithinAbs (-1.0, 1e-6));
    REQUIRE (analyzer.report().broadband.flagged);
    analyzer.prepare (96000.0, { 16, 4.0f, 50.0f });
    REQUIRE (analyzer.report().warming);
    REQUIRE (analyzer.report().broadband.flagged);
    REQUIRE_THAT (analyzer.report().broadband.minimumCorrelation, WithinAbs (-1.0, 1e-6));
    analyzer.resetHeld();
    REQUIRE_FALSE (analyzer.report().broadband.flagged);
    REQUIRE_FALSE (analyzer.report().broadband.minimumValid);
    for (const auto& band : analyzer.report().bands)
        REQUIRE_FALSE (band.flagged);
}

TEST_CASE ("Phase analyzer sizes low-frequency windows and marks unavailable Nyquist bands", "[mastering][phase]")
{
    PhaseAnalyzer analyzer;
    analyzer.prepare (32000.0, { 64, 8.0f, 50.0f });
    REQUIRE (analyzer.report().windowSeconds >= 1.2);
    REQUIRE (analyzer.report().coherenceTimeSeconds >= 8.0 * analyzer.report().windowSeconds);
    REQUIRE_FALSE (analyzer.report().bands[63].available);
    REQUIRE_THAT (analyzer.report().bands[0].lowHz, WithinAbs (20.0, 1e-6));
    REQUIRE_THAT (analyzer.report().bands[63].highHz, WithinAbs (20000.0, 1e-6));
    analyzer.prepare (192000.0, { 64, 8.0f, 5000.0f });
    REQUIRE (analyzer.report().bands[63].available);
    REQUIRE (192000.0 / analyzer.windowSize() <= 20.0 / 8.0 / 3.0);
    analyzer.prepare (std::numeric_limits<double>::quiet_NaN(), { -1, 0.0f, 0.0f });
    REQUIRE_THAT (analyzer.report().sampleRate, WithinAbs (44100.0, 1e-6));
    REQUIRE (analyzer.report().settings.bands == 4);
    REQUIRE_THAT (analyzer.report().settings.q, WithinAbs (0.25, 1e-6));
    REQUIRE_THAT (analyzer.report().settings.averagingMs, WithinAbs (50.0, 1e-6));
}

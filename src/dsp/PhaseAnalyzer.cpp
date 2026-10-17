#include "PhaseAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace duskstudio
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kEnergyFloor = 1.0e-12;
constexpr double kWarningHoldSeconds = 0.25;
constexpr int kMaxBinStep = 3;

double magnitudeSquaredCoherence (double left, double right, std::complex<double> cross)
{
    return left > 0.0 && right > 0.0 ? std::clamp (std::norm (cross) / (left * right), 0.0, 1.0) : 0.0;
}
}

void PhaseAnalyzer::prepare (double rate, MasteringPhaseSettings settings)
{
    rate = std::isfinite (rate) && rate > 0.0 ? rate : 44100.0;
    settings = settings.sanitized();
    const bool preserveHeld = size != 0 && settings.bands == current.settings.bands;
    const bool rebuild = size == 0 || rate != current.sampleRate
                         || settings.bands != current.settings.bands || settings.q != current.settings.q;
    current.settings = settings;
    current.sampleRate = rate;
    if (rebuild)
    {
        int order = dusk::audio::Fft::kMinOrder;
        while (order < 20 && (double) (1 << order) < 3.0 * rate * settings.q / 20.0)
            ++order;
        size = 1 << order;
        fft = std::make_unique<dusk::audio::Fft> (order);
        ringL.resize ((size_t) size);
        ringR.resize ((size_t) size);
        window.resize ((size_t) size);
        workL.assign ((size_t) size * 2, 0.0f);
        workR.assign ((size_t) size * 2, 0.0f);
        correlationSpectrum.resize ((size_t) size / 2 + 1);
        coherenceSpectrum.resize (correlationSpectrum.size());
        dusk::audio::Fft::fillHannWindow (window.data(), size);
        double windowEnergy = 0.0;
        for (const auto value : window) windowEnergy += (double) value * value;
        normalization = 1.0 / (size * windowEnergy);
        fft->performRealOnlyForwardTransform (workL.data(), true);

        const double binHz = rate / size;
        const int firstBin = (int) std::min ((double) (size / 2 + 1), std::max (1.0, std::ceil (20.0 / binHz)));
        const int endBin = (int) std::min ((double) (size / 2 + 1), std::floor (20000.0 / binHz) + 1.0);
        for (int i = 0; i < settings.bands; ++i)
        {
            auto& reading = current.bands[(size_t) i];
            clearCurrent (reading);
            reading.lowHz = 20.0 * std::pow (1000.0, (double) i / settings.bands);
            reading.highHz = 20.0 * std::pow (1000.0, (double) (i + 1) / settings.bands);
            reading.centerHz = std::sqrt (reading.lowHz * reading.highHz);
            reading.available = reading.centerHz < rate * 0.5 && firstBin < endBin;
            auto& band = bands[(size_t) i];
            band.firstBin = (int) std::clamp (std::ceil (reading.centerHz * std::exp (-2.0 / settings.q) / binHz),
                                             (double) firstBin, (double) std::max (firstBin, endBin));
            const int bandEnd = (int) std::clamp (std::floor (reading.centerHz * std::exp (2.0 / settings.q) / binHz) + 1.0,
                                                 (double) band.firstBin, (double) std::max (band.firstBin, endBin));
            band.weights.clear();
            for (int bin = band.firstBin; bin < bandEnd && reading.available; ++bin)
            {
                const double distance = 2.0 * settings.q * std::log (bin * binHz / reading.centerHz);
                const double weight = std::abs (distance) <= 4.0
                                          ? std::exp (-0.5 * distance * distance) : 0.0;
                band.weights.push_back (weight);
            }
        }
        clearCurrent (current.broadband);
        current.broadband.lowHz = 20.0;
        current.broadband.highHz = std::min (20000.0, rate * 0.5);
        current.broadband.available = firstBin < endBin;
        broadband.firstBin = firstBin;
        broadband.weights.assign ((size_t) std::max (0, endBin - firstBin), 1.0);
        if (! preserveHeld) resetHeld();
        markDiscontinuity();
    }
    current.windowSeconds = (double) size / rate;
    current.coherenceTimeSeconds = std::max (settings.averagingMs * 0.001, 8.0 * current.windowSeconds);
    correlationAlpha = std::exp (-(double) hopSize() / rate / (settings.averagingMs * 0.001));
    coherenceAlpha = std::exp (-current.windowSeconds / current.coherenceTimeSeconds);
}

void PhaseAnalyzer::clearCurrent (Reading& reading) noexcept
{
    const auto previous = reading;
    reading = {};
    reading.lowHz = previous.lowHz;
    reading.highHz = previous.highHz;
    reading.centerHz = previous.centerHz;
    reading.available = previous.available;
    reading.minimumCorrelation = previous.minimumCorrelation;
    reading.minimumValid = previous.minimumValid;
    reading.flagged = previous.flagged;
}

void PhaseAnalyzer::markDiscontinuity()
{
    std::fill (correlationSpectrum.begin(), correlationSpectrum.end(), Moments {});
    std::fill (coherenceSpectrum.begin(), coherenceSpectrum.end(), CoherenceBin {});
    writePosition = filled = sinceFrame = 0;
    current.frames = 0;
    current.effectiveCoherenceAverages = 0.0;
    current.warming = true;
    clearCurrent (current.broadband);
    broadband.belowSeconds = 0.0;
    for (size_t i = 0; i < bands.size(); ++i)
    {
        clearCurrent (current.bands[i]);
        bands[i].belowSeconds = 0.0;
    }
}

void PhaseAnalyzer::resetHeld() noexcept
{
    current.broadband.minimumValid = current.broadband.flagged = false;
    current.broadband.minimumCorrelation = 1.0;
    broadband.belowSeconds = 0.0;
    for (size_t i = 0; i < bands.size(); ++i)
    {
        current.bands[i].minimumValid = current.bands[i].flagged = false;
        current.bands[i].minimumCorrelation = 1.0;
        bands[i].belowSeconds = 0.0;
    }
}

void PhaseAnalyzer::push (const float* left, const float* right, int samples)
{
    if (samples <= 0 || left == nullptr || right == nullptr || size == 0) return;
    for (int i = 0; i < samples; ++i)
        if (! std::isfinite (left[i]) || ! std::isfinite (right[i]))
        {
            markDiscontinuity();
            return;
        }
    for (int i = 0; i < samples; ++i)
    {
        ringL[(size_t) writePosition] = left[i];
        ringR[(size_t) writePosition] = right[i];
        writePosition = (writePosition + 1) & (size - 1);
        filled = std::min (size, filled + 1);
        ++sinceFrame;
        if (filled == size && (current.frames == 0 || sinceFrame >= hopSize()))
        {
            analyze();
            sinceFrame = 0;
        }
    }
}

void PhaseAnalyzer::analyze()
{
    for (int i = 0; i < size; ++i)
    {
        const auto slot = (size_t) ((writePosition + i) & (size - 1));
        workL[(size_t) i] = ringL[slot] * window[(size_t) i];
        workR[(size_t) i] = ringR[slot] * window[(size_t) i];
    }
    fft->performRealOnlyForwardTransform (workL.data(), true);
    fft->performRealOnlyForwardTransform (workR.data(), true);
    const bool independentFrame = current.frames % 8 == 0;
    const double alpha = current.frames == 0 ? 0.0 : correlationAlpha;
    for (int bin = 0; bin <= size / 2; ++bin)
    {
        const std::complex<double> l { workL[(size_t) bin * 2], workL[(size_t) bin * 2 + 1] };
        const std::complex<double> r { workR[(size_t) bin * 2], workR[(size_t) bin * 2 + 1] };
        if (! std::isfinite (l.real()) || ! std::isfinite (l.imag())
            || ! std::isfinite (r.real()) || ! std::isfinite (r.imag()))
        {
            markDiscontinuity();
            return;
        }
        const double scale = normalization * (bin == 0 || bin == size / 2 ? 1.0 : 2.0);
        const Moments instant { std::norm (l) * scale, std::norm (r) * scale, l * std::conj (r) * scale };
        auto& correlated = correlationSpectrum[(size_t) bin];
        correlated.left = alpha * correlated.left + (1.0 - alpha) * instant.left;
        correlated.right = alpha * correlated.right + (1.0 - alpha) * instant.right;
        correlated.cross = alpha * correlated.cross + (1.0 - alpha) * instant.cross;
        if (independentFrame)
        {
            auto& accumulated = coherenceSpectrum[(size_t) bin];
            auto& coherent = accumulated.moments;
            coherent.left = coherenceAlpha * coherent.left + (1.0 - coherenceAlpha) * instant.left;
            coherent.right = coherenceAlpha * coherent.right + (1.0 - coherenceAlpha) * instant.right;
            coherent.cross = coherenceAlpha * coherent.cross + (1.0 - coherenceAlpha) * instant.cross;
            const double added = (1.0 - coherenceAlpha) * std::sqrt (instant.left * instant.right);
            accumulated.weight = coherenceAlpha * accumulated.weight + added;
            accumulated.squaredWeight = coherenceAlpha * coherenceAlpha * accumulated.squaredWeight + added * added;
        }
    }
    ++current.frames;
    current.warming = false;
    for (int i = 0; i < current.settings.bands; ++i)
        measure (current.bands[(size_t) i], bands[(size_t) i]);
    measure (current.broadband, broadband);
    current.effectiveCoherenceAverages = current.broadband.effectiveCoherenceAverages;
}

void PhaseAnalyzer::measure (Reading& reading, Band& band)
{
    clearCurrent (reading);
    if (! reading.available) return;
    Moments sum;
    std::complex<double> coherentCross;
    double coherentEnergy = 0.0, coherenceSum = 0.0, peakEnergy = 0.0;
    double matureEnergy = 0.0, effectiveAverages = 0.0, coherentFrequency = 0.0;
    const double binHz = current.sampleRate / size;
    for (size_t i = 0; i < band.weights.size(); ++i)
    {
        const double w = band.weights[i];
        if (w == 0.0) continue;
        const auto bin = (size_t) band.firstBin + i;
        const auto& moment = correlationSpectrum[bin];
        sum.left += w * moment.left;
        sum.right += w * moment.right;
        sum.cross += w * moment.cross;
        const auto& accumulated = coherenceSpectrum[bin];
        const auto& coherent = accumulated.moments;
        const double energy = w * std::sqrt (coherent.left * coherent.right);
        coherentEnergy += energy;
        const double averages = accumulated.squaredWeight > 0.0
                                    ? accumulated.weight * accumulated.weight / accumulated.squaredWeight : 0.0;
        effectiveAverages += energy * averages;
        if (averages >= 6.0)
        {
            matureEnergy += energy;
            coherenceSum += energy * magnitudeSquaredCoherence (coherent.left, coherent.right, coherent.cross);
            coherentCross += w * coherent.cross;
            coherentFrequency += energy * bin * binHz;
        }
        peakEnergy = std::max (peakEnergy, energy);
    }
    const double energy = sum.left + sum.right;
    reading.monoValid = energy > kEnergyFloor;
    if (reading.monoValid)
        reading.monoLossDb = 10.0 * std::log10 (std::clamp ((energy + 2.0 * sum.cross.real()) / (2.0 * energy), 1e-12, 1.0));
    reading.correlationValid = sum.left > kEnergyFloor && sum.right > kEnergyFloor;
    if (! reading.correlationValid)
    {
        band.belowSeconds = 0.0;
        return;
    }
    reading.correlation = std::clamp (sum.cross.real() / std::sqrt (sum.left * sum.right), -1.0, 1.0);
    reading.minimumCorrelation = reading.minimumValid ? std::min (reading.minimumCorrelation, reading.correlation)
                                                     : reading.correlation;
    reading.minimumValid = true;
    band.belowSeconds = reading.correlation < 0.0 ? band.belowSeconds + (double) hopSize() / current.sampleRate : 0.0;
    reading.flagged = reading.flagged || band.belowSeconds >= kWarningHoldSeconds;
    reading.effectiveCoherenceAverages = coherentEnergy > 0.0 ? effectiveAverages / coherentEnergy : 0.0;
    reading.coherenceValid = coherentEnergy > kEnergyFloor && matureEnergy >= 0.5 * coherentEnergy;
    if (! reading.coherenceValid) return;
    reading.coherence = std::clamp (coherenceSum / matureEnergy, 0.0, 1.0);
    reading.phaseValid = reading.coherence >= 0.8 && std::abs (coherentCross) > matureEnergy * 1e-3;
    if (reading.phaseValid)
    {
        const double phase = std::arg (coherentCross);
        reading.phaseDegrees = phase * 180.0 / kPi;
        reading.equivalentDelayMs = phase * 1000.0 / (2.0 * kPi * coherentFrequency / matureEnergy);
    }
    // Delay is a phase slope, including an intercept for polarity. A tone's
    // Hann main lobe is not independent bandwidth for estimating that slope.
    struct Fit
    {
        double weight = 0.0, squaredWeight = 0.0, x = 0.0, y = 0.0, xx = 0.0, xy = 0.0, yy = 0.0;
        double previousPhase = 0.0, unwrapped = 0.0;
        int lastBin = -1;
    } run, best;
    for (size_t i = 0; i < band.weights.size(); ++i)
    {
        const auto bin = (size_t) band.firstBin + i;
        const auto& accumulated = coherenceSpectrum[bin];
        const auto& moment = accumulated.moments;
        const double averages = accumulated.squaredWeight > 0.0
                                    ? accumulated.weight * accumulated.weight / accumulated.squaredWeight : 0.0;
        const double w = band.weights[i] * std::sqrt (moment.left * moment.right);
        if (averages < 6.0 || w < peakEnergy * 1e-3 || w <= 0.0
            || magnitudeSquaredCoherence (moment.left, moment.right, moment.cross) < 0.8)
            continue;
        // Holes within a Hann main lobe do not split a resolved region.
        if (run.lastBin >= 0 && (int) bin - run.lastBin > kMaxBinStep)
        {
            if (run.weight > best.weight) best = run;
            run = {};
        }
        const double phase = std::arg (moment.cross);
        run.unwrapped = run.weight > 0.0 ? run.unwrapped + std::remainder (phase - run.previousPhase, 2.0 * kPi) : phase;
        run.previousPhase = phase;
        run.lastBin = (int) bin;
        const double frequency = bin * binHz;
        run.weight += w;
        run.squaredWeight += w * w;
        run.x += w * frequency;
        run.y += w * run.unwrapped;
        run.xx += w * frequency * frequency;
        run.xy += w * frequency * run.unwrapped;
        run.yy += w * run.unwrapped * run.unwrapped;
    }
    if (run.weight > best.weight) best = run;
    if (best.weight < 0.7 * coherentEnergy || best.squaredWeight <= 0.0
        || best.weight * best.weight / best.squaredWeight < 6.0) return;
    const double variance = best.xx - best.x * best.x / best.weight;
    if (variance / best.weight < 4.0 * binHz * binHz) return;
    const double slope = (best.xy - best.x * best.y / best.weight) / variance;
    if (std::abs (slope) * binHz * kMaxBinStep >= kPi) return;
    const double residual = std::max (0.0, (best.yy - best.y * best.y / best.weight - slope * slope * variance) / best.weight);
    if (residual > 0.15 * 0.15) return;
    reading.delayMs = slope * 1000.0 / (2.0 * kPi);
    reading.delayValid = true;
}
} // namespace duskstudio

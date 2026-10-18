#pragma once

#include "../foundation/Fft.h"
#include "../session/MasteringPhaseParams.h"

#include <array>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace duskstudio
{
// Single message-thread owner. The audio callback feeds PhaseAnalysisFeed;
// neither construction, configuration nor analysis belongs on that callback.
class PhaseAnalyzer
{
public:
    struct Reading
    {
        double lowHz = 0.0, highHz = 0.0, centerHz = 0.0;
        double correlation = 0.0, minimumCorrelation = 1.0;
        double coherence = 0.0, phaseDegrees = 0.0;
        double equivalentDelayMs = 0.0, delayMs = 0.0, monoLossDb = 0.0;
        double effectiveCoherenceAverages = 0.0;
        bool available = false, correlationValid = false, minimumValid = false;
        bool coherenceValid = false, phaseValid = false, delayValid = false;
        bool monoValid = false, flagged = false;
    };

    struct Report
    {
        MasteringPhaseSettings settings;
        std::array<Reading, MasteringPhaseSettings::kMaxBands> bands {};
        Reading broadband;
        double sampleRate = 0.0, windowSeconds = 0.0, coherenceTimeSeconds = 0.0;
        double effectiveCoherenceAverages = 0.0;
        std::uint64_t frames = 0;
        bool warming = true;
    };

    void prepare (double sampleRate, MasteringPhaseSettings settings = {});
    void push (const float* left, const float* right, int samples);
    void markDiscontinuity();
    void resetHeld() noexcept;

    const Report& report() const noexcept { return current; }
    int windowSize() const noexcept { return size; }
    int hopSize() const noexcept { return size / 8; }

private:
    struct Moments
    {
        double left = 0.0, right = 0.0;
        std::complex<double> cross {};
    };

    struct Band
    {
        int firstBin = 0;
        std::vector<double> weights;
        double belowSeconds = 0.0;
    };

    struct CoherenceBin
    {
        Moments moments;
        double weight = 0.0, squaredWeight = 0.0;
    };

    void analyze();
    void measure (Reading& reading, Band& band);
    void clearCurrent (Reading& reading) noexcept;

    Report current;
    std::unique_ptr<dusk::audio::Fft> fft;
    std::vector<float> ringL, ringR, window, workL, workR;
    std::vector<Moments> correlationSpectrum;
    std::vector<CoherenceBin> coherenceSpectrum;
    std::array<Band, MasteringPhaseSettings::kMaxBands> bands;
    Band broadband;
    int size = 0, writePosition = 0, filled = 0, sinceFrame = 0;
    double normalization = 0.0, correlationAlpha = 0.0, coherenceAlpha = 0.0;
};
} // namespace duskstudio

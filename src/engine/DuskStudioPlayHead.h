#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Transport.h"
#include "TransportSnapshot.h"
#include "../session/Session.h"
#include <atomic>
#include <cmath>

namespace duskstudio
{
// AudioPlayHead reporting Dusk Studio's transport state, BPM, and timeline
// position to hosted plugins. Tempo-synced features (LFOs, arpeggiators,
// delays with bar/beat divisions, tape-style transport-driven UIs) all
// query this. Without a playhead set, JUCE-hosted plugins fall back to a
// default 120 BPM regardless of session tempo.
class DuskStudioPlayHead final : public juce::AudioPlayHead
{
public:
    DuskStudioPlayHead (Transport& t,
                       const std::atomic<float>* bpm,
                       const std::atomic<double>* sampleRate,
                       const AtomicSnapshot<TempoMap>* tempoMap = nullptr,
                       const std::atomic<int>* numerator = nullptr,
                       const std::atomic<int>* denominator = nullptr) noexcept
        : transport (t), bpmSource (bpm), sampleRateSource (sampleRate),
          tempoMapSource (tempoMap), numeratorSource (numerator), denominatorSource (denominator) {}

    juce::Optional<PositionInfo> getPosition() const override
    {
        const double bpm = (bpmSource != nullptr)
                            ? (double) bpmSource->load (std::memory_order_relaxed)
                            : 120.0;
        const double sr = (sampleRateSource != nullptr)
                            ? sampleRateSource->load (std::memory_order_relaxed)
                            : 0.0;
        const int numerator = numeratorSource != nullptr
                            ? numeratorSource->load (std::memory_order_relaxed) : 4;
        const int denominator = denominatorSource != nullptr
                              ? denominatorSource->load (std::memory_order_relaxed) : 4;
        const SnapshotReadScope snapshotReads;
        auto position = snapshotTransport (transport, bpm, sr, numerator, denominator);
        if (tempoMapSource != nullptr)
            applyTransportTempoMap (position, tempoMapSource->read(), sr, kMidiTicksPerQuarter);

        PositionInfo info;
        info.setIsPlaying   (position.isPlaying);
        info.setIsRecording (position.isRecording);
        info.setIsLooping   (position.isLooping);
        info.setBpm (position.bpm);

        info.setTimeSignature (TimeSignature {
            position.timeSignatureNumerator, position.timeSignatureDenominator });

        info.setTimeInSamples (position.timeInSamples);
        if (std::isfinite (sr) && sr > 0.0)
        {
            info.setTimeInSeconds (position.timeInSeconds);
            if (position.bpm > 0.0)
            {
                info.setPpqPosition (position.ppqPosition);
                const double quartersPerBar = (double) position.timeSignatureNumerator * 4.0
                                             / (double) position.timeSignatureDenominator;
                info.setPpqPositionOfLastBarStart (std::floor (position.ppqPosition / quartersPerBar) * quartersPerBar);
                info.setLoopPoints (LoopPoints { position.ppqLoopStart, position.ppqLoopEnd });
            }
        }

        return info;
    }

private:
    Transport& transport;
    const std::atomic<float>*  bpmSource;
    const std::atomic<double>* sampleRateSource;
    const AtomicSnapshot<TempoMap>* tempoMapSource;
    const std::atomic<int>* numeratorSource;
    const std::atomic<int>* denominatorSource;
};
} // namespace duskstudio

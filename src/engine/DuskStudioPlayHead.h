#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../foundation/AtomicTransportPosition.h"
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
    explicit DuskStudioPlayHead (const dusk::AtomicTransportPosition& source) noexcept
        : snapshotSource (source) {}

    juce::Optional<PositionInfo> getPosition() const override
    {
        const auto snapshot = snapshotSource.read();
        if (! snapshot) return {};
        const auto& position = *snapshot;

        PositionInfo info;
        info.setIsPlaying   (position.isPlaying || position.isRecording);
        info.setIsRecording (position.isRecording);
        if (std::isfinite (position.bpm) && position.bpm >= 0.0)
            info.setBpm (position.bpm);

        const bool signatureValid = position.timeSignatureNumerator > 0
                                 && position.timeSignatureDenominator > 0;
        if (signatureValid)
            info.setTimeSignature (TimeSignature {
                position.timeSignatureNumerator, position.timeSignatureDenominator });

        info.setTimeInSamples (position.timeInSamples);
        if (std::isfinite (position.timeInSeconds))
            info.setTimeInSeconds (position.timeInSeconds);
        const bool musicalPositionValid = std::isfinite (position.bpm) && position.bpm > 0.0
                                       && std::isfinite (position.ppqPosition);
        const bool loopValid = musicalPositionValid && std::isfinite (position.ppqLoopStart)
                            && std::isfinite (position.ppqLoopEnd)
                            && position.ppqLoopEnd > position.ppqLoopStart;
        info.setIsLooping (position.isLooping && loopValid);
        if (musicalPositionValid)
        {
            info.setPpqPosition (position.ppqPosition);
            if (signatureValid)
            {
                const double quartersPerBar = (double) position.timeSignatureNumerator * 4.0
                                             / (double) position.timeSignatureDenominator;
                const double barStart = std::floor (position.ppqPosition / quartersPerBar) * quartersPerBar;
                if (std::isfinite (barStart)) info.setPpqPositionOfLastBarStart (barStart);
            }
            if (loopValid)
                info.setLoopPoints (LoopPoints { position.ppqLoopStart, position.ppqLoopEnd });
        }

        return info;
    }

private:
    const dusk::AtomicTransportPosition& snapshotSource;
};
} // namespace duskstudio

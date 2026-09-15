#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Transport.h"
#include "TransportSnapshot.h"
#include <atomic>
#include <cmath>

namespace duskstudio
{
// AudioPlayHead reporting Dusk Studio's transport state, BPM, and timeline
// position to hosted plugins. Tempo-synced features (LFOs, arpeggiators,
// delays with bar/beat divisions, tape-style transport-driven UIs) all
// query this. Without a playhead set, JUCE-hosted plugins fall back to a
// default 120 BPM regardless of session tempo.
//
// `bpmSource` is a non-owning pointer to Session::tempoBpm; the audio
// thread reads it lock-free via memory_order_relaxed (UI mutates with
// the same ordering on tempo changes). nullptr is allowed - the play-
// head reports a fallback 120 BPM in that case.
//
// `sampleRateSource` is a pointer to AudioEngine::currentSampleRate for
// converting the playhead's sample position into wall-clock seconds.
class DuskStudioPlayHead final : public juce::AudioPlayHead
{
public:
    DuskStudioPlayHead (Transport& t,
                   const std::atomic<float>* bpm,
                   const std::atomic<double>* sampleRate) noexcept
        : transport (t), bpmSource (bpm), sampleRateSource (sampleRate) {}

    juce::Optional<PositionInfo> getPosition() const override
    {
        const double bpm = (bpmSource != nullptr)
                            ? (double) bpmSource->load (std::memory_order_relaxed)
                            : 120.0;
        const double sr = (sampleRateSource != nullptr)
                            ? sampleRateSource->load (std::memory_order_relaxed)
                            : 0.0;
        const auto position = snapshotTransport (transport, bpm, sr);

        PositionInfo info;
        info.setIsPlaying   (position.isPlaying);
        info.setIsRecording (position.isRecording);
        info.setBpm (position.bpm);

        // Plugins do better with 4/4 than with the default 0/0.
        info.setTimeSignature (juce::AudioPlayHead::TimeSignature {
            position.timeSignatureNumerator, position.timeSignatureDenominator });

        info.setTimeInSamples (position.timeInSamples);
        if (sr > 0.0)
            info.setTimeInSeconds (position.timeInSeconds);

        // Beats-per-bar of 4 feeds setPpqPositionOfLastBarStart for plugins
        // that need bar-start.
        if (sr > 0.0 && bpm > 0.0)
        {
            info.setPpqPosition (position.ppqPosition);
            info.setPpqPositionOfLastBarStart (std::floor (position.ppqPosition / 4.0) * 4.0);
        }

        return info;
    }

private:
    Transport& transport;
    const std::atomic<float>*  bpmSource;
    const std::atomic<double>* sampleRateSource;
};
} // namespace duskstudio

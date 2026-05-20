#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Transport.h"
#include <atomic>

namespace duskstudio
{
// AudioPlayHead reporting Focal's transport state, BPM, and timeline
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
        PositionInfo info;
        info.setIsPlaying   (transport.isPlaying());
        info.setIsRecording (transport.isRecording());

        const double bpm = (bpmSource != nullptr)
                            ? (double) bpmSource->load (std::memory_order_relaxed)
                            : 120.0;
        info.setBpm (bpm);

        // 4/4 by default; Focal doesn't currently expose a session-wide
        // time signature but plugins do better with a sane value than
        // with the default 0/0.
        info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });

        const auto playheadSamples = transport.getPlayhead();
        info.setTimeInSamples (playheadSamples);

        const double sr = (sampleRateSource != nullptr)
                            ? sampleRateSource->load (std::memory_order_relaxed)
                            : 0.0;
        if (sr > 0.0)
            info.setTimeInSeconds ((double) playheadSamples / sr);

        // PPQ position from samples + bpm + sample rate. ppq = beats =
        // samples * bpm / (60 * sr). Beats-per-bar of 4 (4/4) feeds
        // setPpqPositionOfLastBarStart for plugins that need bar-start.
        if (sr > 0.0 && bpm > 0.0)
        {
            const double ppq = (double) playheadSamples * bpm / (60.0 * sr);
            info.setPpqPosition (ppq);
            const double barStart = std::floor (ppq / 4.0) * 4.0;
            info.setPpqPositionOfLastBarStart (barStart);
        }

        return info;
    }

private:
    Transport& transport;
    const std::atomic<float>*  bpmSource;
    const std::atomic<double>* sampleRateSource;
};
} // namespace duskstudio

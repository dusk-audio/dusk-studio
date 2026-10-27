#pragma once

#include "Transport.h"
#include "../foundation/TransportPosition.h"
#include <cmath>

namespace duskstudio
{
// The transport a hosted plug-in sees for one block, from the engine's transport,
// the session tempo and the device rate. It uses the committed audio position;
// a seek requested during this block belongs to the next one.
inline dusk::TransportPosition snapshotTransport (Transport::State state,
                                                  std::int64_t samples,
                                                  double bpm, double sampleRate,
                                                  int numerator, int denominator,
                                                  bool loopEnabled,
                                                  std::int64_t loopStart,
                                                  std::int64_t loopEnd) noexcept
{
    dusk::TransportPosition position;
    position.isPlaying     = state == Transport::State::Playing || state == Transport::State::Recording;
    position.isRecording   = state == Transport::State::Recording;
    position.bpm           = std::isfinite (bpm) && bpm >= 0.0 ? bpm : 120.0;
    position.timeInSamples = samples;
    position.timeSignatureNumerator   = numerator > 0 ? numerator : 4;
    position.timeSignatureDenominator = denominator > 0 ? denominator : 4;
    position.loopStartSamples = loopStart;
    position.loopEndSamples   = loopEnd;
    position.isLooping = loopEnabled && loopEnd > loopStart;
    if (std::isfinite (sampleRate) && sampleRate > 0.0)
    {
        position.timeInSeconds = (double) position.timeInSamples / sampleRate;
        const double quartersPerSample = position.bpm / (60.0 * sampleRate);
        position.ppqPosition  = (double) samples * quartersPerSample;
        position.ppqLoopStart = (double) loopStart * quartersPerSample;
        position.ppqLoopEnd   = (double) loopEnd * quartersPerSample;
    }
    return position;
}

inline dusk::TransportPosition snapshotTransport (const Transport& transport,
                                                  double bpm, double sampleRate,
                                                  int numerator = 4, int denominator = 4) noexcept
{
    return snapshotTransport (transport.getState(), transport.getCommittedPlayhead(),
                              bpm, sampleRate, numerator, denominator,
                              transport.isLoopEnabled(), transport.getLoopStart(), transport.getLoopEnd());
}

template <typename Map>
inline void applyTransportTempoMap (dusk::TransportPosition& position,
                                    const Map* map, double sampleRate,
                                    double ticksPerQuarter) noexcept
{
    if (map == nullptr || map->empty()) return;
    position.bpm = (double) map->bpmAt (position.timeInSamples);
    if (! std::isfinite (sampleRate) || sampleRate <= 0.0
        || ! std::isfinite (ticksPerQuarter) || ticksPerQuarter <= 0.0)
        return;

    const auto ppqAt = [&] (std::int64_t sample) noexcept
    {
        // TempoMap clamps negative positions to zero; plug-ins must retain pre-roll.
        return sample < 0 ? (double) sample * (double) map->bpmAt (0) / (60.0 * sampleRate)
                          : map->samplesToTicksFractional (sample, sampleRate) / ticksPerQuarter;
    };
    position.ppqPosition  = ppqAt (position.timeInSamples);
    position.ppqLoopStart = ppqAt (position.loopStartSamples);
    position.ppqLoopEnd   = ppqAt (position.loopEndSamples);
}
} // namespace duskstudio

#pragma once

#include "Transport.h"
#include "../foundation/TransportPosition.h"

namespace duskstudio
{
// The transport a hosted plug-in sees for one block, from the engine's transport,
// the session tempo and the device rate. Dusk Studio has no time signature yet, so
// every plug-in is told 4/4 (TransportPosition's default). Audio-thread safe: the
// transport reads are relaxed atomic loads.
inline dusk::TransportPosition snapshotTransport (const Transport& transport,
                                                  double bpm, double sampleRate) noexcept
{
    dusk::TransportPosition position;
    position.isPlaying     = transport.isPlaying();
    position.isRecording   = transport.isRecording();
    position.bpm           = bpm;
    position.timeInSamples = transport.getPlayhead();
    if (sampleRate > 0.0)
        position.timeInSeconds = (double) position.timeInSamples / sampleRate;
    if (sampleRate > 0.0 && bpm > 0.0)
        position.ppqPosition = (double) position.timeInSamples * bpm / (60.0 * sampleRate);
    return position;
}
} // namespace duskstudio

#pragma once

#include "Transport.h"
#include "../foundation/TransportPosition.h"

namespace duskstudio
{
// The transport a hosted plug-in sees for one block, from the engine's transport,
// the session tempo and the device rate. It uses the committed audio position;
// a seek requested during this block belongs to the next one.
inline dusk::TransportPosition snapshotTransport (const Transport& transport,
                                                  double bpm, double sampleRate) noexcept
{
    dusk::TransportPosition position;
    position.isPlaying     = transport.isPlaying();
    position.isRecording   = transport.isRecording();
    position.bpm           = bpm;
    position.timeInSamples = transport.getCommittedPlayhead();
    if (sampleRate > 0.0)
        position.timeInSeconds = (double) position.timeInSamples / sampleRate;
    if (sampleRate > 0.0 && bpm > 0.0)
        position.ppqPosition = (double) position.timeInSamples * bpm / (60.0 * sampleRate);
    return position;
}
} // namespace duskstudio

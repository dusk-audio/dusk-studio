#pragma once

#include "foundation/MidiBuffer.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace duskstudio::test
{
// Bridge a juce::MidiBuffer into the dusk::MidiBuffer the receivers take,
// exactly as AudioEngine does per block. Shared by the MIDI-sync round-trip
// tests so they feed the receivers the way the engine does.
inline dusk::MidiBuffer toDusk (const juce::MidiBuffer& j)
{
    dusk::MidiBuffer d;
    for (const auto meta : j)
        d.addEvent (meta.getMessage().getRawData(), meta.getMessage().getRawDataSize(),
                    meta.samplePosition);
    return d;
}
} // namespace duskstudio::test

#pragma once

#include <juce_core/juce_core.h>

namespace duskstudio::pdc
{
// Cross-track PDC compensation math, factored out of AudioEngine::recomputePdc
// so it can be unit-tested without the engine. `latency[i]` is track i's
// effective insert latency in samples (0 for MIDI tracks — their instrument
// latency is absorbed by the MIDI scheduling pre-shift — and 0 for empty
// inserts). Fills `comp[i] = deepest - latency[i]` (≥ 0, the samples track i
// must be delayed to line up with the deepest track) and returns the deepest
// latency (the session-wide lead-in the bounce must trim).
inline int computeCompensations (const int* latency, int* comp, int n) noexcept
{
    int deepest = 0;
    for (int i = 0; i < n; ++i)
        deepest = juce::jmax (deepest, latency[i]);
    for (int i = 0; i < n; ++i)
        comp[i] = deepest - latency[i];
    return deepest;
}
} // namespace duskstudio::pdc

#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace duskstudio::appconfig
{
// Per-machine preferences. Backed by a juce::PropertiesFile at
//   <userApplicationDataDirectory>/Focal/app-config.properties
// - separate store from window-state.txt (geometry) and recent.txt
// (recent sessions list) so each file has a single concern.
//
// All accessors are message-thread only. The PropertiesFile has its own
// internal locking, but values returned here aren't atomic on their own,
// so don't poll from the audio thread.

// User-controlled UI scale multiplier on top of JUCE's per-display DPI.
// Default 1.0 (no extra zoom). Clamped to [0.5, 2.0] on write - the
// outer rails of "still readable" on either end.
constexpr float kUiScaleMin     = 0.5f;
constexpr float kUiScaleMax     = 2.0f;
constexpr float kUiScaleDefault = 1.0f;

float getUiScaleOverride();
void  setUiScaleOverride (float scale);
} // namespace duskstudio::appconfig

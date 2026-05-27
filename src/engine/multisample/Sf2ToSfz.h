#pragma once

#include "Sf2Reader.h"

#include <juce_core/juce_core.h>

namespace duskstudio
{
// Converts one preset of a SoundFont 2 file into an SFZ text body plus a
// directory of extracted WAV samples, so the already-vendored sfizz
// engine can play it (sfizz has no native SF2 reader). This removes the
// fluidsynth dependency for SF2 playback.
//
// Generator layering follows the SF2 2.04 articulation model: an
// instrument's global zone supplies defaults that each instrument zone
// overrides; the preset's global + per-instrument zones then apply on
// top (additive for tune/attenuation/pan, intersecting for key/vel
// ranges). Envelopes are handled in a later pass.
struct Sf2Conversion
{
    bool         ok { false };
    juce::String error;
    juce::String sfzText;     // synthetic SFZ body for sfizz_load_string
    juce::File   sampleDir;   // dir holding the extracted WAVs (sfizz root path)
    juce::String presetName;  // human-readable, for the editor's file label
};

// Convert preset `presetIndex` of `sf2` into SFZ + WAVs under `outDir`
// (created if needed; caller owns cleanup). presetIndex is clamped into
// range. Only samples referenced by the chosen preset are extracted.
Sf2Conversion convertSf2Preset(const juce::File& sf2,
                                int               presetIndex,
                                const juce::File& outDir);
} // namespace duskstudio

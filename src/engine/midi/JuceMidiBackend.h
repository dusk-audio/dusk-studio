#pragma once

#include "MidiBackend.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <memory>

// JUCE-backed MIDI backend for platforms without a native one (macOS, Windows).
// Linux uses AlsaSeqMidi and never compiles this TU, so the JUCE MIDI device
// API - including the juce::AudioDeviceManager the input side needs for its
// enable/callback lifecycle - is confined here rather than leaking into the
// seam or the engine.
namespace duskstudio::midi
{
// Must be called before the input backend is built. The manager outlives the
// backend (the engine owns both).
void setJuceMidiDeviceManager (juce::AudioDeviceManager& dm);

std::unique_ptr<IMidiInputBackend>  makeJuceMidiInputBackend();
std::unique_ptr<IMidiOutputBackend> makeJuceMidiOutputBackend();
} // namespace duskstudio::midi

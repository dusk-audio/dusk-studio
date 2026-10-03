#pragma once

#include "MidiBackend.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <memory>

// JUCE-backed MIDI backend for platforms without a native one (macOS, Windows).
// The Linux app uses AlsaSeqMidi and never compiles this TU, so the JUCE MIDI
// device API - including JUCE's AudioDeviceManager, which the input side needs for
// its enable/callback lifecycle - is confined here rather than leaking into the
// seam or the engine.
namespace duskstudio::midi
{
// The input backend enables, attaches and detaches through deviceManager for
// its whole life, so the manager must outlive it (the engine owns both). Each
// backend keeps the manager it was built with: a process can hold two engines,
// and the scenario suite destroys its second while the first still runs. With
// no manager the backend enumerates but never enables or delivers an input.
std::unique_ptr<IMidiInputBackend>  makeJuceMidiInputBackend (juce::AudioDeviceManager* deviceManager);
std::unique_ptr<IMidiOutputBackend> makeJuceMidiOutputBackend();
} // namespace duskstudio::midi

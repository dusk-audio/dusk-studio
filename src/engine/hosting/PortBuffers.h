#pragma once

#include <juce_audio_basics/juce_audio_basics.h>   // MidiBuffer, AudioPlayHead::PositionInfo

namespace duskstudio::hosting
{
// The single audio-thread argument to INativeInstance::processBlock, replacing
// the CLAP host's fixed (inL, inR, outL, outR, numFrames). Every pointer refers
// to storage the caller/instance sized ahead of time — NOTHING here is allocated
// or locked on the audio thread. Channel arrays are `float* const*` (an array of
// per-channel pointers), so mono / stereo / multi-out all use one shape.
//
// juce::MidiBuffer and AudioPlayHead::PositionInfo are the engine's existing
// currency for MIDI and transport; each host translates them into its own format
// (VST3 Event, LV2 atom-midi, CLAP note events) internally.
struct PortBuffers
{
    // Main audio I/O. The audio arrays are mutable (float* const*) because
    // in-place-capable hosts — CLAP's clap_audio_buffer.data32, VST3's
    // ProcessData — hand the plugin writable input buffers; the InsertAdapter
    // owns and refills that storage each block, so a plugin scribbling on its
    // input is harmless. The const is on the array, not the samples.
    float* const* mainIn          = nullptr;
    int           mainInChannels  = 0;
    float* const* mainOut         = nullptr;
    int           mainOutChannels = 0;

    // Optional sidechain input. When a slot could take a sidechain but none is
    // routed, the InsertAdapter feeds pre-sized silence rather than a null
    // pointer — some plugins dereference their sidechain unconditionally.
    float* const* sidechainIn         = nullptr;
    int           sidechainInChannels = 0;

    int numFrames = 0;

    // Optional MIDI. midiIn drives instrument / MIDI-effect inserts; midiOut
    // collects a plugin's emitted MIDI (null until a destination is wired).
    const juce::MidiBuffer* midiIn  = nullptr;
    juce::MidiBuffer*       midiOut = nullptr;

    // Optional transport for tempo-synced plugins (null = not supplied).
    const juce::AudioPlayHead::PositionInfo* transport = nullptr;
};
} // namespace duskstudio::hosting

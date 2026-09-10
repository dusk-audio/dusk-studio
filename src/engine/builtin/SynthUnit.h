#pragma once

#include "BuiltinUnit.h"

#include <memory>

namespace msynth { class MultiSynthDSP; }

namespace duskstudio::builtin
{
// Subtractive / FM / acid synth over the donor Sunset Circuits core, which is
// framework-free C++17 and seeds all 228 of its parameters from its own table
// in its constructor. This unit drives the two dozen a player reaches for and
// leaves the rest at the donor's init patch.
//
// The core has no per-event timing: noteOn / noteOff take no sample offset, so
// the block's MIDI is applied at block granularity ahead of the render. At the
// engine's block sizes that is well under a millisecond of quantisation and it
// is what the core's own plugin shells do.
//
// It runs a fixed internal 2x, set in its own prepare, so there is no factor
// for the engine to drive and nothing here switches oversampling on.
class SynthUnit final : public BuiltinUnit
{
public:
    SynthUnit();
    ~SynthUnit() override;

    void prepare (double sampleRate, int maxBlockFrames) override;
    void process (float* left, float* right, int numFrames,
                  const dusk::MidiBuffer* midi) noexcept override;

    // Message thread. Silences every sounding voice, for a slot being torn down
    // while notes are held.
    void releaseAllNotes() noexcept;

private:
    enum ParamIndex
    {
        kMode = 0, kMasterVolDb, kMasterTuneCents, kPitchBendRange, kPortamento,
        kUnisonVoices, kUnisonDetuneCents,
        kOsc1Wave, kOsc1Level, kOsc2Wave, kOsc2Level, kOsc2DetuneCents, kOsc2Semi,
        kSubLevel, kNoiseLevel,
        kFilterCutoffHz, kFilterRes, kFilterEnvAmt,
        kAmpAttack, kAmpDecay, kAmpSustain, kAmpRelease,
        kFiltAttack, kFiltDecay, kFiltSustain, kFiltRelease,
        kNumParams
    };

    void applyMidi (const dusk::MidiBuffer& midi) noexcept;

    std::unique_ptr<msynth::MultiSynthDSP> core;
    int maxFrames = 0;
};
} // namespace duskstudio::builtin

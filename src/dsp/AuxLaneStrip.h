#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>
#include "../session/Session.h"
#include "../engine/PluginSlot.h"
#include "HardwareInsertSlot.h"

namespace duskstudio
{
// AUX return lane DSP. Hosts up to AuxLaneParams::kMaxLanePlugins plugin
// slots in series, applies a return-level gain, and writes stereo output
// meters. The lane's input buffer is the per-channel send accumulation done
// in ChannelStrip::processAndAccumulate; this class processes that buffer
// in place and the AudioEngine sums it into master after the bus pass.
//
// AuxLaneStrip is structurally lighter than BusStrip (no EQ, no comp, no
// pan): the EQ/comp role on a return is the plugin's job - typical FX
// returns are a reverb or a delay, not a bus subgroup.
class AuxLaneStrip
{
public:
    static constexpr int kMaxPlugins = AuxLaneParams::kMaxLanePlugins;

    AuxLaneStrip() = default;

    void prepare (double sampleRate, int blockSize);
    void bind (const AuxLaneParams& params) noexcept
    {
        paramsRef = &params;
        // Seed the smoother to the actual return level so the first block
        // doesn't ramp from unity → bound value (audible level jump on
        // session load when a lane was saved at e.g. -10 dB).
        const float db = params.returnLevelDb.load (std::memory_order_relaxed);
        const float gain = (db <= ChannelStripParams::kFaderInfThreshDb)
                             ? 0.0f
                             : juce::Decibels::decibelsToGain (db);
        returnGain.setCurrentAndTargetValue (gain);
    }

    // Bind the per-app PluginManager so this lane's slots can resolve plugin
    // files. Mirrors the channel-strip binding done at engine construction.
    void bindPluginManager (PluginManager& mgr) noexcept
    {
        for (auto& s : slots) s.setManager (mgr);
    }

    PluginSlot&       getPluginSlot (int idx)       noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return slots[(size_t) idx]; }
    const PluginSlot& getPluginSlot (int idx) const noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return slots[(size_t) idx]; }

    // Hardware-insert side of each slot. Slot's audio mode (Plugin vs
    // Hardware vs empty) is chosen by insertMode[slotIdx]; mode flips
    // crossfade through activeInsertGain[slotIdx] over ~20 ms.
    void bindHardwareInsert (int slotIdx, const HardwareInsertParams& params) noexcept;
    HardwareInsertSlot&       getHardwareInsertSlot (int idx)       noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return hardwareSlots[(size_t) idx]; }
    const HardwareInsertSlot& getHardwareInsertSlot (int idx) const noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return hardwareSlots[(size_t) idx]; }

    enum InsertMode : int { kInsertEmpty = 0, kInsertPlugin = 1, kInsertHardware = 2 };

    // Per-slot insert mode. Default plugin (current behaviour). Audio
    // thread reads with acquire.
    std::array<std::atomic<int>, kMaxPlugins> insertMode {};

    // Audio-thread API. Buffer holds the summed channel sends (already
    // pan-aware stereo). Runs each loaded plugin in series, applies the
    // return-level fader, writes meter atoms. When the lane is muted, the
    // buffer is cleared and processing is skipped.
    //
    // deviceInputs / deviceOutputs are forwarded to HardwareInsertSlot
    // for the hardware-insert side of each slot (wired in Phase 3).
    // Defaulted to null/0 so non-engine call sites still compile.
    void processStereoBlock (float* L, float* R, int numSamples,
                              const float* const* deviceInputs  = nullptr,
                              int   numDeviceInputs             = 0,
                              float* const*       deviceOutputs = nullptr,
                              int   numDeviceOutputs            = 0) noexcept;

private:
    const AuxLaneParams* paramsRef = nullptr;
    juce::SmoothedValue<float> returnGain { 1.0f };

    std::array<PluginSlot, kMaxPlugins> slots;

    // Hardware-insert side of each slot (parallel to `slots`). Either
    // slots[s] or hardwareSlots[s] runs in any given block; the gate
    // below cross-fades on mode flips.
    std::array<HardwareInsertSlot, kMaxPlugins> hardwareSlots;

    // Audio-thread crossfade state, one per slot. activeInsertMode[s]
    // is what is CURRENTLY running; insertMode[s] is what the UI wants.
    // Mismatch triggers a ramp-out / mode-swap / ramp-in cycle through
    // activeInsertGain[s].
    std::array<int,                          kMaxPlugins> activeInsertMode {};
    std::array<juce::SmoothedValue<float>,   kMaxPlugins> activeInsertGain;

    // Pre-insert scratch (per channel) used so the crossfade can blend
    // pre vs post on a per-sample basis without allocating per-call.
    std::vector<float> insertScratchL;
    std::vector<float> insertScratchR;

    // Empty MIDI buffer threaded through the slots' processStereoBlock calls.
    // Aux lanes host effect plugins only, so this stays empty - kept as a
    // member purely so the audio thread doesn't default-construct one.
    juce::MidiBuffer pluginMidiScratch;

    void updateGainTarget() noexcept;
};
} // namespace duskstudio

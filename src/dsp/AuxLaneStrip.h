#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>
#include "../session/Session.h"
#include "../engine/PluginSlot.h"
#include "HardwareInsertSlot.h"

namespace duskstudio
{
// AUX return lane. Hosts up to AuxLaneParams::kMaxLanePlugins slots in
// series + return-level gain + stereo meters. Lane input is the
// per-channel send accumulation; this processes in place and
// AudioEngine sums into master after the bus pass.
// Lighter than BusStrip (no EQ / no comp / no pan) — the plugin is the
// EQ / comp / character on a return.
class AuxLaneStrip
{
public:
    static constexpr int kMaxPlugins = AuxLaneParams::kMaxLanePlugins;

    AuxLaneStrip() = default;

    void prepare (double sampleRate, int blockSize);
    void bind (const AuxLaneParams& params) noexcept
    {
        paramsRef = &params;
        // Seed the smoother so the first block doesn't ramp unity ->
        // bound value (audible jump on load when lane saved at e.g. -10 dB).
        const float db = params.returnLevelDb.load (std::memory_order_relaxed);
        const float gain = (db <= ChannelStripParams::kFaderInfThreshDb)
                             ? 0.0f
                             : juce::Decibels::decibelsToGain (db);
        returnGain.setCurrentAndTargetValue (gain);
    }

    void bindPluginManager (PluginManager& mgr) noexcept
    {
        for (auto& s : slots) s.setManager (mgr);
    }

    PluginSlot&       getPluginSlot (int idx)       noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return slots[(size_t) idx]; }
    const PluginSlot& getPluginSlot (int idx) const noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return slots[(size_t) idx]; }

    // Plugin vs Hardware vs empty per slot; mode flips crossfade
    // through activeInsertGain[slotIdx] over ~20 ms.
    void bindHardwareInsert (int slotIdx, const HardwareInsertParams& params) noexcept;
    HardwareInsertSlot&       getHardwareInsertSlot (int idx)       noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return hardwareSlots[(size_t) idx]; }
    const HardwareInsertSlot& getHardwareInsertSlot (int idx) const noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return hardwareSlots[(size_t) idx]; }

    enum InsertMode : int { kInsertEmpty = 0, kInsertPlugin = 1, kInsertHardware = 2 };

    std::array<std::atomic<int>, kMaxPlugins> insertMode {};

    // Mute clears L/R and skips processing.
    // deviceInputs/Outputs forwarded to HardwareInsertSlot. Defaulted
    // to null so non-engine callers still compile.
    void processStereoBlock (float* L, float* R, int numSamples,
                              const float* const* deviceInputs  = nullptr,
                              int   numDeviceInputs             = 0,
                              float* const*       deviceOutputs = nullptr,
                              int   numDeviceOutputs            = 0) noexcept;

private:
    const AuxLaneParams* paramsRef = nullptr;
    juce::SmoothedValue<float> returnGain { 1.0f };

    std::array<PluginSlot, kMaxPlugins> slots;
    std::array<HardwareInsertSlot, kMaxPlugins> hardwareSlots;

    // activeInsertMode[s] = currently running; insertMode[s] = UI target.
    // Mismatch triggers ramp-out / swap / ramp-in via activeInsertGain[s].
    std::array<int,                          kMaxPlugins> activeInsertMode {};
    std::array<juce::SmoothedValue<float>,   kMaxPlugins> activeInsertGain;

    std::vector<float> insertScratchL;
    std::vector<float> insertScratchR;

    // Aux hosts effects only — buffer stays empty. Held as member so
    // the audio thread never default-constructs one.
    juce::MidiBuffer pluginMidiScratch;

    void updateGainTarget() noexcept;
};
} // namespace duskstudio

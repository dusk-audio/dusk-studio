#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>
#include "../session/Session.h"
#include "../engine/PluginSlot.h"
#if DUSKSTUDIO_HAS_NATIVE_CLAP
  #include "../engine/clap/NativeClapSlot.h"   // Linux-only native CLAP host
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
  #include "../engine/lv2/NativeLv2Slot.h"     // Linux-only native LV2 host
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
  #include "../engine/vst3/NativeVst3Slot.h"   // Linux-only native VST3 host
#endif
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

    // Native CLAP host path. When a slot's native CLAP is loaded, processStereoBlock
    // routes the kInsertPlugin pass through it INSTEAD of the JUCE PluginSlot — this
    // is the JUCE-hosting replacement (see docs/native-clap-host-plan.md). Empty by
    // default, so a lane with no native CLAP behaves exactly as before. Message thread.
    bool isPrepared() const noexcept { return preparedSampleRate > 0.0 && preparedBlockSize > 0; }

    // Linux-only native CLAP host (DUSKSTUDIO_HAS_NATIVE_CLAP); stubbed elsewhere so the
    // bool/void API still compiles. getNativeClapSlot returns a clap type → Linux-only.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    bool loadNativeClap   (int slotIdx, const juce::File& path, std::string& errorOut,
                           const juce::String& pluginId = {});
    void unloadNativeClap (int slotIdx) noexcept;
    bool isNativeClapLoaded (int slotIdx) const noexcept { jassert (slotIdx >= 0 && slotIdx < kMaxPlugins); return nativeClapSlots[(size_t) slotIdx].isLoaded(); }
    clap::NativeClapSlot&       getNativeClapSlot (int idx)       noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return nativeClapSlots[(size_t) idx]; }
    const clap::NativeClapSlot& getNativeClapSlot (int idx) const noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return nativeClapSlots[(size_t) idx]; }

    // Session restore: load() needs the sample rate, but consumePluginStateAfterLoad
    // can run before the engine is prepared. Stash the saved {path, state}; prepare()
    // loads it once the SR is known. (When already prepared, the engine loads directly
    // and never calls this.)
    void setPendingNativeClap (int slotIdx, const juce::File& path, std::vector<uint8_t> state,
                               const juce::String& pluginId = {}) noexcept;

    // True when a sample-rate re-prepare reload of a loaded native CLAP failed (the
    // slot is now empty). The UI reads this to report which lane lost its plugin.
    bool nativeClapReloadFailed (int slotIdx) const noexcept { jassert (slotIdx >= 0 && slotIdx < kMaxPlugins); return nativeReloadFailed[(size_t) slotIdx].load (std::memory_order_relaxed); }
    // See ChannelStrip::markNativeClapRestoreFailed — re-marks a failed engine restore.
    void markNativeClapRestoreFailed (int slotIdx) noexcept { jassert (slotIdx >= 0 && slotIdx < kMaxPlugins); nativeReloadFailed[(size_t) slotIdx].store (true, std::memory_order_relaxed); }
#else
    bool isNativeClapLoaded (int) const noexcept { return false; }
    void unloadNativeClap (int) noexcept {}
    bool nativeClapReloadFailed (int) const noexcept { return false; }
#endif

    // Native LV2 host path — same contract as the CLAP block above.
#if DUSKSTUDIO_HAS_NATIVE_LV2
    bool loadNativeLv2   (int slotIdx, const juce::File& path, std::string& errorOut,
                          const juce::String& pluginId = {});
    void unloadNativeLv2 (int slotIdx) noexcept;
    bool isNativeLv2Loaded (int slotIdx) const noexcept { jassert (slotIdx >= 0 && slotIdx < kMaxPlugins); return nativeLv2Slots[(size_t) slotIdx].isLoaded(); }
    lv2::NativeLv2Slot&       getNativeLv2Slot (int idx)       noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return nativeLv2Slots[(size_t) idx]; }
    const lv2::NativeLv2Slot& getNativeLv2Slot (int idx) const noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return nativeLv2Slots[(size_t) idx]; }
    void setPendingNativeLv2 (int slotIdx, const juce::File& path, std::vector<uint8_t> state,
                              const juce::String& pluginId = {}) noexcept;
    bool nativeLv2ReloadFailed (int slotIdx) const noexcept { jassert (slotIdx >= 0 && slotIdx < kMaxPlugins); return lv2ReloadFailed[(size_t) slotIdx].load (std::memory_order_relaxed); }
    void markNativeLv2RestoreFailed (int slotIdx) noexcept { jassert (slotIdx >= 0 && slotIdx < kMaxPlugins); lv2ReloadFailed[(size_t) slotIdx].store (true, std::memory_order_relaxed); }
#else
    bool isNativeLv2Loaded (int) const noexcept { return false; }
    void unloadNativeLv2 (int) noexcept {}
    bool nativeLv2ReloadFailed (int) const noexcept { return false; }
#endif

    // Native VST3 host path — same contract as the CLAP block above.
#if DUSKSTUDIO_HAS_NATIVE_VST3
    bool loadNativeVst3   (int slotIdx, const juce::File& path, std::string& errorOut,
                           const juce::String& pluginId = {});
    void unloadNativeVst3 (int slotIdx) noexcept;
    bool isNativeVst3Loaded (int slotIdx) const noexcept { jassert (slotIdx >= 0 && slotIdx < kMaxPlugins); return nativeVst3Slots[(size_t) slotIdx].isLoaded(); }
    vst3::NativeVst3Slot&       getNativeVst3Slot (int idx)       noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return nativeVst3Slots[(size_t) idx]; }
    const vst3::NativeVst3Slot& getNativeVst3Slot (int idx) const noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return nativeVst3Slots[(size_t) idx]; }
    void setPendingNativeVst3 (int slotIdx, const juce::File& path, std::vector<uint8_t> state,
                               const juce::String& pluginId = {}) noexcept;
    bool nativeVst3ReloadFailed (int slotIdx) const noexcept { jassert (slotIdx >= 0 && slotIdx < kMaxPlugins); return vst3ReloadFailed[(size_t) slotIdx].load (std::memory_order_relaxed); }
    void markNativeVst3RestoreFailed (int slotIdx) noexcept { jassert (slotIdx >= 0 && slotIdx < kMaxPlugins); vst3ReloadFailed[(size_t) slotIdx].store (true, std::memory_order_relaxed); }
#else
    bool isNativeVst3Loaded (int) const noexcept { return false; }
    void unloadNativeVst3 (int) noexcept {}
    bool nativeVst3ReloadFailed (int) const noexcept { return false; }
#endif

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
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    std::array<clap::NativeClapSlot, kMaxPlugins> nativeClapSlots;
    std::array<std::atomic<bool>,    kMaxPlugins> nativeReloadFailed {};

    // Pending session-restore load, consummated by prepare() (see setPendingNativeClap).
    std::array<juce::String,           kMaxPlugins> pendingClapPath;
    std::array<juce::String,           kMaxPlugins> pendingClapPluginId;
    std::array<std::vector<uint8_t>,   kMaxPlugins> pendingClapState;
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    std::array<lv2::NativeLv2Slot,   kMaxPlugins> nativeLv2Slots;
    std::array<std::atomic<bool>,    kMaxPlugins> lv2ReloadFailed {};
    std::array<juce::String,         kMaxPlugins> pendingLv2Path;
    std::array<juce::String,         kMaxPlugins> pendingLv2PluginId;
    std::array<std::vector<uint8_t>, kMaxPlugins> pendingLv2State;
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    std::array<vst3::NativeVst3Slot, kMaxPlugins> nativeVst3Slots;
    std::array<std::atomic<bool>,    kMaxPlugins> vst3ReloadFailed {};
    std::array<juce::String,         kMaxPlugins> pendingVst3Path;
    std::array<juce::String,         kMaxPlugins> pendingVst3PluginId;
    std::array<std::vector<uint8_t>, kMaxPlugins> pendingVst3State;
#endif

    // Stashed in prepare so loadNativeClap (and re-prepare across a sample-rate
    // change) can (re)activate a native CLAP at the engine's current spec.
    double preparedSampleRate = 0.0;
    int    preparedBlockSize  = 0;

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

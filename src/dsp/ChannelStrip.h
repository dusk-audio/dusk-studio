#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <vector>
#include "../foundation/IntDelayLine.h"
#include "../foundation/MidiBuffer.h"
#include "../foundation/SmoothedValue.h"
#include "../foundation/StereoOversampler.h"
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

#if DUSKSTUDIO_HAS_DUSK_DSP
  // Framework-free donor cores. The strip drives the full comp feature set
  // (Opto/FET/VCA) with the core's internal oversampling left off because the
  // strip wraps EQ+Comp in its own StereoOversampler - running both would
  // double-OS. The EQ likewise runs 1x internally inside that same wrap (its
  // console saturation is the only saturating stage and the wrap band-limits it).
  #include <dsp/FourKEQDSP.hpp>
  #include <core/UniversalCompressorDSP.hpp>
#endif

namespace duskstudio
{
// 4K-style HPF + 4-band EQ + FET/Opto/VCA comp + per-aux sends + pan +
// fader + SIP gating.
class ChannelStrip
{
public:
    static constexpr int kNumBuses    = ChannelStripParams::kNumBuses;
    static constexpr int kNumAuxSends = ChannelStripParams::kNumAuxSends;

    void prepare (double sampleRate, int blockSize, int oversamplingFactor = 1);
    void bind (const ChannelStripParams& params) noexcept { paramsRef = &params; }

    // Cross-track Plugin Delay Compensation. The engine's PDC aggregator
    // (AudioEngine::recomputePdc) sets each strip's compensation = the session's
    // deepest track latency minus this track's own, so every track lines up on
    // every route (master / buses / aux). Message thread only.
    static constexpr int kMaxPdcSamples = 16384;   // ~340 ms @ 48k, matches HW insert
    void setPdcCompensationSamples (int n) noexcept
    {
        pdcTargetSamples.store (std::clamp (n, 0, kMaxPdcSamples), std::memory_order_relaxed);
    }
    int getPdcCompensationSamples() const noexcept
    {
        return pdcTargetSamples.load (std::memory_order_relaxed);
    }

    // Slot is dormant until a plugin loads - empty strips pay zero RT cost.
    void bindPluginManager (PluginManager& mgr) noexcept { pluginSlot.setManager (mgr); }
    PluginSlot&       getPluginSlot()       noexcept { return pluginSlot; }
    const PluginSlot& getPluginSlot() const noexcept { return pluginSlot; }

    // Native CLAP host path (replaces JUCE hosting for this insert when loaded). When
    // a native CLAP is loaded, the insert pass runs through it instead of pluginSlot.
    // Mirrors AuxLaneStrip. Message thread; engine fences load/unload via its gate.
    // Linux-only (DUSKSTUDIO_HAS_NATIVE_CLAP); stubbed elsewhere so callers compile.
    bool isPrepared() const noexcept { return preparedSampleRate > 0.0 && preparedBlockSize > 0; }
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    bool loadNativeClap   (const juce::File& path, std::string& errorOut,
                           const juce::String& pluginId = {});
    void unloadNativeClap() noexcept;
    bool isNativeClapLoaded() const noexcept { return nativeClapSlot.isLoaded(); }
    clap::NativeClapSlot&       getNativeClapSlot()       noexcept { return nativeClapSlot; }
    const clap::NativeClapSlot& getNativeClapSlot() const noexcept { return nativeClapSlot; }
    // Session restore before the engine is prepared: stash {path,state}; prepare() loads it.
    void setPendingNativeClap (const juce::File& path, std::vector<uint8_t> state,
                               const juce::String& pluginId = {}) noexcept;
    // True when the last prepare()-time reactivate / pending-restore load failed. Lets
    // the save path tell "failed restore" (keep the persisted path) from "user removed".
    bool nativeClapReloadFailed() const noexcept { return nativeReloadFailed.load (std::memory_order_relaxed); }
    // Engine session-restore failure: loadNativeClap clears the flag (it treats every
    // call as user-initiated), so a failed RESTORE must re-mark it to keep the refs.
    void markNativeClapRestoreFailed() noexcept { nativeReloadFailed.store (true, std::memory_order_relaxed); }
#else
    bool isNativeClapLoaded() const noexcept { return false; }
    void unloadNativeClap() noexcept {}
    bool nativeClapReloadFailed() const noexcept { return false; }
#endif

    // Native LV2 host path - same contract as the CLAP block above.
#if DUSKSTUDIO_HAS_NATIVE_LV2
    bool loadNativeLv2   (const juce::File& path, std::string& errorOut,
                          const juce::String& pluginId = {});
    void unloadNativeLv2() noexcept;
    bool isNativeLv2Loaded() const noexcept { return nativeLv2Slot.isLoaded(); }
    lv2::NativeLv2Slot&       getNativeLv2Slot()       noexcept { return nativeLv2Slot; }
    const lv2::NativeLv2Slot& getNativeLv2Slot() const noexcept { return nativeLv2Slot; }
    void setPendingNativeLv2 (const juce::File& path, std::vector<uint8_t> state,
                              const juce::String& pluginId = {},
                              const std::filesystem::path& stateDir = {}) noexcept;
    bool nativeLv2ReloadFailed() const noexcept { return lv2ReloadFailed.load (std::memory_order_relaxed); }
    void markNativeLv2RestoreFailed() noexcept { lv2ReloadFailed.store (true, std::memory_order_relaxed); }
#else
    bool isNativeLv2Loaded() const noexcept { return false; }
    void unloadNativeLv2() noexcept {}
    bool nativeLv2ReloadFailed() const noexcept { return false; }
#endif

    // Native VST3 host path - same contract as the CLAP block above.
#if DUSKSTUDIO_HAS_NATIVE_VST3
    bool loadNativeVst3   (const juce::File& path, std::string& errorOut,
                           const juce::String& pluginId = {});
    void unloadNativeVst3() noexcept;
    bool isNativeVst3Loaded() const noexcept { return nativeVst3Slot.isLoaded(); }
    vst3::NativeVst3Slot&       getNativeVst3Slot()       noexcept { return nativeVst3Slot; }
    const vst3::NativeVst3Slot& getNativeVst3Slot() const noexcept { return nativeVst3Slot; }
    void setPendingNativeVst3 (const juce::File& path, std::vector<uint8_t> state,
                               const juce::String& pluginId = {}) noexcept;
    bool nativeVst3ReloadFailed() const noexcept { return vst3ReloadFailed.load (std::memory_order_relaxed); }
    void markNativeVst3RestoreFailed() noexcept { vst3ReloadFailed.store (true, std::memory_order_relaxed); }
#else
    bool isNativeVst3Loaded() const noexcept { return false; }
    void unloadNativeVst3() noexcept {}
    bool nativeVst3ReloadFailed() const noexcept { return false; }
#endif

    // Whether a native host owns the insert with an instrument loaded (no main
    // audio input - MIDI drives it). Gates the track-mode/unload interplay.
    bool insertIsNativeInstrument() const noexcept
    {
#if DUSKSTUDIO_HAS_NATIVE_CLAP
        if (isNativeClapLoaded()) return nativeClapSlot.isLoadedInstrument();
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
        if (isNativeLv2Loaded()) return nativeLv2Slot.isLoadedInstrument();
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
        if (isNativeVst3Loaded()) return nativeVst3Slot.isLoadedInstrument();
#endif
        return false;
    }

    // MIDI Learn: last-touched parameter of whichever host owns the insert
    // (same precedence as the audio chain); -1 when empty or untouched.
    int insertLastTouchedParamIndex() const noexcept
    {
#if DUSKSTUDIO_HAS_NATIVE_CLAP
        if (isNativeClapLoaded()) return nativeClapSlot.lastTouchedParamIndex();
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
        if (isNativeLv2Loaded()) return nativeLv2Slot.lastTouchedParamIndex();
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
        if (isNativeVst3Loaded()) return nativeVst3Slot.lastTouchedParamIndex();
#endif
        return pluginSlot.getLastTouchedParamIndex();
    }

    // Hardware vs plugin insert: one runs per block, chosen by
    // insertMode + 20 ms crossfade gate.
    void bindHardwareInsert (const HardwareInsertParams& params) noexcept;
    HardwareInsertSlot&       getHardwareInsertSlot()       noexcept { return hardwareSlot; }
    const HardwareInsertSlot& getHardwareInsertSlot() const noexcept { return hardwareSlot; }

    enum InsertMode : int { kInsertEmpty = 0, kInsertPlugin = 1, kInsertHardware = 2 };

    // Message thread writes; audio thread reads with acquire and starts
    // a 20 ms crossfade.
    std::atomic<int> insertMode { kInsertPlugin };

    // Engine sets true when the recorder will read getLastProcessedMono
    // this block (armed && recording && printEffects). False allows the
    // strip to skip DSP when also passByGate==false.
    void setNeedsProcessedMono (bool needed) noexcept { needsProcessedMono = needed; }

    float getCurrentGrDb() const noexcept { return currentGrDb.load (std::memory_order_relaxed); }

    // Post-fader / post-pan output peak (dB) for this block - the track's
    // contribution to the mix, regardless of bus/master routing. The strip
    // meter shows this during playback (switches to the pre-fader input meter
    // when the track is input-monitoring). -100 = silent / skipped.
    float getOutLDb() const noexcept { return currentOutLDb.load (std::memory_order_relaxed); }
    float getOutRDb() const noexcept { return currentOutRDb.load (std::memory_order_relaxed); }

    // Valid for lastProcessedSamples samples after the most recent
    // processAndAccumulate. nullptr if no DSP ran this block.
    const float* getLastProcessedMono() const noexcept { return lastProcessedPtr; }
    // nullptr in mono mode - callers wanting stereo print must check.
    const float* getLastProcessedR() const noexcept { return lastProcessedR; }
    int getLastProcessedSamples() const noexcept { return lastProcessedSamples; }

    // inR == nullptr for mono. isMidi=true sources audio from the loaded
    // instrument plugin, ignoring inL/inR; trackMidi carries per-track
    // filtered events. Accumulates post-fader into masterL/R when not
    // bus-routed, into busL/R[N] for each assigned bus, into auxLaneL/R[N]
    // at auxSendDb[N], pre- or post-fader per auxSendPreFader[N].
    //
    // deviceInputs/Outputs are forwarded to the hardware-insert slot.
    // Defaulted to null so non-engine callers (tests) still compile.
    void processAndAccumulate (const float* inL,
                               const float* inR,
                               juce::MidiBuffer& trackMidi,
                               bool  isMidi,
                               float* masterL, float* masterR,
                               const std::array<float*, kNumBuses>& busL,
                               const std::array<float*, kNumBuses>& busR,
                               const std::array<float*, kNumAuxSends>& auxLaneL,
                               const std::array<float*, kNumAuxSends>& auxLaneR,
                               int numSamples,
                               bool passByGate,
                               const float* const* deviceInputs  = nullptr,
                               int   numDeviceInputs             = 0,
                               float* const*       deviceOutputs = nullptr,
                               int   numDeviceOutputs            = 0,
                               bool  isFrozen                    = false) noexcept;

    // Track freeze. When a frozen track plays back, inL/inR carry the
    // pre-rendered (instrument + EQ + comp) audio, so the strip skips the
    // instrument plugin and the EQ/comp stage (they're baked into the WAV)
    // but keeps PDC + fader/pan/sends live. Pass isFrozen=true above.
    //
    // setFreezeCapture points the strip at a stereo scratch the offline
    // freeze render reads each block: after the EQ/comp stage (pre-fader),
    // srcL/srcR are copied there. nullptr (the default) disables the tap, so
    // it costs nothing on the live path. Message thread only, set while the
    // engine is detached for the offline render - never during live audio.
    void setFreezeCapture (float* l, float* r) noexcept { freezeCapL = l; freezeCapR = r; }

    // setStemCapture points the strip at a stereo scratch the stems render
    // reads each block: the post-fader / post-pan output (the track's full
    // wet contribution, BEFORE the master-vs-bus routing split) is
    // accumulated there. The caller clears the scratch each block, so a
    // strip whose processing is skipped leaves silence. Message thread;
    // atomic (unlike the freeze tap) because a REALTIME bounce arms it while
    // the live callback is running.
    void setStemCapture (float* l, float* r) noexcept
    {
        // R first: the audio thread gates on L, so L's release-store publishes R.
        stemCapR.store (r, std::memory_order_relaxed);
        stemCapL.store (l, std::memory_order_release);
    }

private:
    const ChannelStripParams* paramsRef = nullptr;
    dusk::audio::SmoothedValue<float> faderGain  { 0.0f };
    dusk::audio::SmoothedValue<float> panGainL   { 0.7071f };
    dusk::audio::SmoothedValue<float> panGainR   { 0.7071f };
    // Binary bus routing smoothed 0..1 to avoid clicks on toggle.
    std::array<dusk::audio::SmoothedValue<float>, kNumBuses> busGain;
    std::array<dusk::audio::SmoothedValue<float>, kNumAuxSends> auxSendGain;
    // Sampled at the top of processAndAccumulate so the inner loop
    // avoids per-sample atomic loads.
    std::array<bool, kNumAuxSends> auxSendPre {};

    std::vector<float> tempMono;

    // 2-channel even on mono so a mid-session mono->stereo flip doesn't
    // fault on missing filter / envelope state. Shared with the plugin /
    // hardware insert hosting path, so it stays a juce::AudioBuffer.
    juce::AudioBuffer<float> tempStereoBuffer;

    // Per-strip Dusk Studio-side oversampler wrapping (EQ + Comp). The donor
    // EQ's always-on console saturation and the comp's saturation alias hard at
    // native rate; the wrap moves the whole per-channel DSP to the oversampled
    // rate so the half-band FIRs band-limit it before downsampling. One
    // StereoOversampler serves both track widths: mono feeds channel 0 with a
    // silent channel 1 (osMonoScratchR) and processes only channel 0 - the
    // wasted R half-band FIR runs on silence, but avoids doubling the expensive
    // EQ + comp work a duplicate-mono approach would incur, and no mono-only
    // primitive exists.
    dusk::audio::StereoOversampler oversampler;
    std::vector<float> osMonoScratchR;
    int oversampleFactor = 1;    // 1 / 2 / 4 - drives the comp sub-chunk size

    // Sits between phase invert and the EQ stage.
    PluginSlot pluginSlot;
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    clap::NativeClapSlot nativeClapSlot;   // native CLAP alternative to pluginSlot
    std::atomic<bool>    nativeReloadFailed { false };   // prepare()-time reactivate/restore failed
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    lv2::NativeLv2Slot nativeLv2Slot;      // native LV2 alternative to pluginSlot
    std::atomic<bool>  lv2ReloadFailed { false };
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    vst3::NativeVst3Slot nativeVst3Slot;   // native VST3 alternative to pluginSlot
    std::atomic<bool>    vst3ReloadFailed { false };
#endif
    HardwareInsertSlot hardwareSlot;

    // Stashed in prepare() so loadNativeClap / pending-restore can (re)activate at spec.
    double preparedSampleRate = 0.0;
    int    preparedBlockSize  = 0;
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    juce::String         pendingClapPath;    // session-restore load consummated by prepare()
    juce::String         pendingClapPluginId;
    std::vector<uint8_t> pendingClapState;
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    juce::String         pendingLv2Path;
    juce::String         pendingLv2PluginId;
    std::vector<uint8_t> pendingLv2State;
    std::filesystem::path pendingLv2StateDir;
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    juce::String         pendingVst3Path;
    juce::String         pendingVst3PluginId;
    std::vector<uint8_t> pendingVst3State;
#endif

    // activeInsertMode = what we're currently running; insertMode = what
    // the UI wants. Mismatch triggers ramp-out / swap / ramp-in.
    int activeInsertMode = kInsertPlugin;
    juce::SmoothedValue<float> activeInsertGain;

    std::vector<float> insertScratchL;
    std::vector<float> insertScratchR;

    // PDC compensation delay (post-insert / pre-EQ). Two single-channel lines
    // mirror the HardwareInsertSlot dry-delay pattern. The delay length only
    // re-latches once the line has drained to silence (pdcSilentRun >= the
    // currently-applied length) so a latency change never clicks. pdcApplied /
    // pdcSilentRun are audio-thread-only; pdcTargetSamples is the cross-thread
    // setpoint.
    dusk::audio::IntDelayLine pdcDelayL;
    dusk::audio::IntDelayLine pdcDelayR;
    std::atomic<int> pdcTargetSamples { 0 };
    int          pdcAppliedSamples = 0;
    std::int64_t  pdcSilentRun = 0;
    void relatchPdcIfDrained (float blockPeakAbs, int numSamples) noexcept;

    // The per-strip oversampler's rounded internal latency (half-band filter
    // state). The EQ - which carries the always-on console saturation - runs
    // every block, so the oversampler is never skipped; this value is kept only
    // so the silent-skip can account for the oversampler's tail before dropping
    // a block (see requiredDrain in processAndAccumulate). The FIR round trip is
    // 23 (2x) / lround(26.5)=27 (4x) base-rate samples.
    static constexpr int kMaxOsLatency = 32;
    int          osLatencySamples = 0;

    // Empty buffer for the channel insert plugin; PluginSlot's
    // processBlock requires a MidiBuffer& even when the insert is an
    // effect. Held as member so the audio thread never default-constructs.
    juce::MidiBuffer pluginMidiScratch;

    // Native hosts (CLAP/LV2/VST3) consume dusk::MidiBuffer; the JUCE PluginSlot
    // path keeps juce::MidiBuffer. This bridges the instrument block's MIDI into
    // dusk once per block. Pre-sized in prepare() so the refill never allocates.
    dusk::MidiBuffer nativeMidiScratch;

#if DUSKSTUDIO_HAS_DUSK_DSP
    duskaudio::FourKEQDSP eq;
    // On false->true we eq.reset() so a disabled-then-re-enabled EQ
    // doesn't emit a transient from stale filter history.
    bool prevEqEnabled { true };

    // memcmp against last block - only push the EQ setters when a field
    // changed, skipping the biquad recompute when no knob moved. Plain-float
    // struct (no padding) value-init to zero so memcmp is byte-reliable, and
    // so a bypassed EQ's flat defaults (all bands 0 dB, filters off) are the
    // literal zero image.
    struct EqSnapshot
    {
        float hpfEnabled = 0, hpfFreq = 0, lpfEnabled = 0, lpfFreq = 0;
        float lfGain = 0, lfFreq = 0, lfBell = 0;
        float lmGain = 0, lmFreq = 0, lmQ = 0;
        float hmGain = 0, hmFreq = 0, hmQ = 0;
        float hfGain = 0, hfFreq = 0, hfBell = 0;
        float eqType = 0, saturation = 0, inputGain = 0, outputGain = 0;
    };
    EqSnapshot lastEqParams {};

    duskaudio::UniversalCompressorDSP compressor;

    // 20 ms ramps for continuous params so knob drags don't zipper.
    // Discrete params (mode, bypass, LIMIT, FET ratio) bypass. Their per-chunk
    // published values feed the core's atomic setters (no APVTS atoms to cache
    // - the core does not port the donor's analog-hiss stage, so the old
    // noise_enable force-off is unnecessary).
    dusk::audio::SmoothedValue<float> smoothedOptoPeakRed;
    dusk::audio::SmoothedValue<float> smoothedOptoGain;
    dusk::audio::SmoothedValue<float> smoothedFetInput;
    dusk::audio::SmoothedValue<float> smoothedFetOutput;
    dusk::audio::SmoothedValue<float> smoothedFetAttack;
    dusk::audio::SmoothedValue<float> smoothedFetRelease;
    dusk::audio::SmoothedValue<float> smoothedFetThreshold;
    dusk::audio::SmoothedValue<float> smoothedVcaThresh;
    dusk::audio::SmoothedValue<float> smoothedVcaRatio;
    dusk::audio::SmoothedValue<float> smoothedVcaAttack;
    dusk::audio::SmoothedValue<float> smoothedVcaRelease;
    dusk::audio::SmoothedValue<float> smoothedVcaOutput;

    void publishSmoothedCompParams (int numSamples) noexcept;
#endif

    std::atomic<float> currentGrDb { 0.0f };
    std::atomic<float> currentOutLDb { -100.0f };
    std::atomic<float> currentOutRDb { -100.0f };
    const float* lastProcessedPtr = nullptr;
    const float* lastProcessedR   = nullptr;
    int          lastProcessedSamples = 0;
    bool         needsProcessedMono = false;

    // Pre-fader capture destinations for the offline freeze render (see
    // setFreezeCapture). nullptr on the live path.
    float* freezeCapL = nullptr;
    float* freezeCapR = nullptr;

    // Post-fader capture destinations for the stems render (see
    // setStemCapture). nullptr when no stems bounce is running.
    std::atomic<float*> stemCapL { nullptr };
    std::atomic<float*> stemCapR { nullptr };

    void updateGainTargets() noexcept;
    void updateEqParameters() noexcept;
    void updateCompParameters() noexcept;
};
} // namespace duskstudio

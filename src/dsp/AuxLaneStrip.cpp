#include "AuxLaneStrip.h"
#include "OutputPairRouting.h"
#include <cmath>

namespace duskstudio
{
void AuxLaneStrip::prepare (double sampleRate, int blockSize)
{
    preparedSampleRate = sampleRate;
    preparedBlockSize  = juce::jmax (1, blockSize);

    // A loaded native CLAP was activated at the prior spec; re-activate at the
    // new one. Reload by path (the instance has no in-place re-prepare) — a
    // sample-rate change resets DSP state anyway. Engine fences this via its
    // process gate, so the audio thread never sees a half-swapped slot.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    for (int s = 0; s < kMaxPlugins; ++s)
    {
        auto& ncs = nativeClapSlots[(size_t) s];
        if (ncs.isLoaded())
        {
            // Re-activate in place (NOT a full reload): a reload destroys the instance
            // the editor's GUI is attached to — plugin->destroy with a live GUI aborts
            // u-he plugins — and resets the plugin's parameters. reactivate keeps the
            // instance + GUI + state and only re-sizes for the new spec.
            std::string err;
            const bool ok = ncs.reactivate (preparedSampleRate, preparedBlockSize, err);
            nativeReloadFailed[(size_t) s].store (! ok, std::memory_order_relaxed);
        }
        else if (pendingClapPath[(size_t) s].isNotEmpty())
        {
            // Pending session-restore load — SR is known now. insertMode was already
            // set to kInsertPlugin by the engine before it stashed this.
            const juce::File p (pendingClapPath[(size_t) s]);
            std::string err;
            const bool ok = ncs.load (p, preparedSampleRate, preparedBlockSize, err,
                                      pendingClapPluginId[(size_t) s]);
            if (ok && ! pendingClapState[(size_t) s].empty())
                ncs.loadState (pendingClapState[(size_t) s]);
            nativeReloadFailed[(size_t) s].store (! ok, std::memory_order_relaxed);
            pendingClapPath[(size_t) s].clear();
            pendingClapPluginId[(size_t) s].clear();
            pendingClapState[(size_t) s].clear();
        }
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    for (int s = 0; s < kMaxPlugins; ++s)
    {
        auto& nls = nativeLv2Slots[(size_t) s];
        if (nls.isLoaded())
        {
            std::string err;
            const bool ok = nls.reactivate (preparedSampleRate, preparedBlockSize, err);
            lv2ReloadFailed[(size_t) s].store (! ok, std::memory_order_relaxed);
        }
        else if (pendingLv2Path[(size_t) s].isNotEmpty())
        {
            if (! isNativeClapLoaded (s))   // both pendings set (corrupt session): CLAP wins
            {
                const juce::File p (pendingLv2Path[(size_t) s]);
                std::string err;
                const bool ok = nls.load (p, preparedSampleRate, preparedBlockSize, err,
                                          pendingLv2PluginId[(size_t) s]);
                if (ok && ! pendingLv2State[(size_t) s].empty())
                {
                    nls.setStateDirectory (pendingLv2StateDir[(size_t) s]);
                    nls.loadState (pendingLv2State[(size_t) s]);
                }
                lv2ReloadFailed[(size_t) s].store (! ok, std::memory_order_relaxed);
            }
            // Consumed either way — a CLAP-suppressed pending must not replay on a
            // later prepare() once the CLAP is unloaded.
            pendingLv2Path[(size_t) s].clear();
            pendingLv2PluginId[(size_t) s].clear();
            pendingLv2State[(size_t) s].clear();
            pendingLv2StateDir[(size_t) s] = juce::File();
        }
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    for (int s = 0; s < kMaxPlugins; ++s)
    {
        auto& nvs = nativeVst3Slots[(size_t) s];
        if (nvs.isLoaded())
        {
            std::string err;
            const bool ok = nvs.reactivate (preparedSampleRate, preparedBlockSize, err);
            vst3ReloadFailed[(size_t) s].store (! ok, std::memory_order_relaxed);
        }
        else if (pendingVst3Path[(size_t) s].isNotEmpty())
        {
            if (! isNativeClapLoaded (s) && ! isNativeLv2Loaded (s))   // several pendings set (corrupt session): CLAP > LV2 > VST3
            {
                const juce::File p (pendingVst3Path[(size_t) s]);
                std::string err;
                const bool ok = nvs.load (p, preparedSampleRate, preparedBlockSize, err,
                                          pendingVst3PluginId[(size_t) s]);
                if (ok && ! pendingVst3State[(size_t) s].empty())
                    nvs.loadState (pendingVst3State[(size_t) s]);
                vst3ReloadFailed[(size_t) s].store (! ok, std::memory_order_relaxed);
            }
            pendingVst3Path[(size_t) s].clear();
            pendingVst3PluginId[(size_t) s].clear();
            pendingVst3State[(size_t) s].clear();
        }
    }
#endif

    constexpr double rampSeconds = 0.020;
    returnGain.reset (sampleRate, rampSeconds);
    returnGain.setCurrentAndTargetValue (1.0f);
    for (auto& s : slots)
        s.prepareToPlay (sampleRate, juce::jmax (1, blockSize));

    // Per-slot hardware insert + crossfade gate. Same 20 ms ramp used
    // by the channel strip so mode flips feel consistent across the
    // app.
    for (auto& hw : hardwareSlots) hw.prepare (sampleRate, blockSize);
    for (size_t s = 0; s < (size_t) kMaxPlugins; ++s)
    {
        activeInsertGain[s].reset (sampleRate, rampSeconds);
        activeInsertGain[s].setCurrentAndTargetValue (1.0f);
        activeInsertMode[s] = insertMode[s].load (std::memory_order_relaxed);
        if (activeInsertMode[s] == 0)
            activeInsertMode[s] = kInsertPlugin;   // preserve existing behaviour
    }
    insertScratchL.assign ((size_t) juce::jmax (1, blockSize), 0.0f);
    insertScratchR.assign ((size_t) juce::jmax (1, blockSize), 0.0f);

    // Pre-size the MIDI scratch so the audio thread's plugin-output
    // addEvent calls (PluginSlot / RemotePluginConnection deserialise)
    // can't grow it. 4 KB matches the channel-strip allotment.
    pluginMidiScratch.ensureSize (4096);
}

void AuxLaneStrip::bindHardwareInsert (int slotIdx, const HardwareInsertParams& params) noexcept
{
    jassert (slotIdx >= 0 && slotIdx < kMaxPlugins);
    hardwareSlots[(size_t) slotIdx].bind (params);
}

#if DUSKSTUDIO_HAS_NATIVE_CLAP
bool AuxLaneStrip::loadNativeClap (int slotIdx, const juce::File& path, std::string& errorOut,
                                   const juce::String& pluginId)
{
    jassert (slotIdx >= 0 && slotIdx < kMaxPlugins);
    if (preparedSampleRate <= 0.0 || preparedBlockSize <= 0)
    { errorOut = "aux lane not prepared"; return false; }
    // One host per slot: evict the other native formats and any JUCE plugin so the
    // audio chain (CLAP → LV2 → VST3 → JUCE) never sees two loaded. Callers fence
    // the audio thread around this call.
    unloadNativeLv2 (slotIdx);
    unloadNativeVst3 (slotIdx);
    slots[(size_t) slotIdx].unload();
    const bool ok = nativeClapSlots[(size_t) slotIdx].load (path, preparedSampleRate, preparedBlockSize, errorOut, pluginId);
    // User-initiated load always ends any "failed restore" state (see ChannelStrip::
    // loadNativeClap) — clear regardless of outcome so the flag stays caller-independent.
    nativeReloadFailed[(size_t) slotIdx].store (false, std::memory_order_relaxed);
    return ok;
}

void AuxLaneStrip::unloadNativeClap (int slotIdx) noexcept
{
    jassert (slotIdx >= 0 && slotIdx < kMaxPlugins);
    nativeClapSlots[(size_t) slotIdx].unload();
    nativeReloadFailed[(size_t) slotIdx].store (false, std::memory_order_relaxed);   // slot reset — clear stale failure
    pendingClapPath[(size_t) slotIdx].clear();
    pendingClapPluginId[(size_t) slotIdx].clear();
    pendingClapState[(size_t) slotIdx].clear();
}

void AuxLaneStrip::setPendingNativeClap (int slotIdx, const juce::File& path,
                                          std::vector<uint8_t> state,
                                          const juce::String& pluginId) noexcept
{
    jassert (slotIdx >= 0 && slotIdx < kMaxPlugins);
    pendingClapPath[(size_t) slotIdx]     = path.getFullPathName();
    pendingClapPluginId[(size_t) slotIdx] = pluginId;
    pendingClapState[(size_t) slotIdx]    = std::move (state);
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_LV2
bool AuxLaneStrip::loadNativeLv2 (int slotIdx, const juce::File& path, std::string& errorOut,
                                  const juce::String& pluginId)
{
    jassert (slotIdx >= 0 && slotIdx < kMaxPlugins);
    if (preparedSampleRate <= 0.0 || preparedBlockSize <= 0)
    { errorOut = "aux lane not prepared"; return false; }
    // One host per slot — see loadNativeClap.
    unloadNativeClap (slotIdx);
    unloadNativeVst3 (slotIdx);
    slots[(size_t) slotIdx].unload();
    const bool ok = nativeLv2Slots[(size_t) slotIdx].load (path, preparedSampleRate, preparedBlockSize, errorOut, pluginId);
    lv2ReloadFailed[(size_t) slotIdx].store (false, std::memory_order_relaxed);
    return ok;
}

void AuxLaneStrip::unloadNativeLv2 (int slotIdx) noexcept
{
    jassert (slotIdx >= 0 && slotIdx < kMaxPlugins);
    nativeLv2Slots[(size_t) slotIdx].unload();
    lv2ReloadFailed[(size_t) slotIdx].store (false, std::memory_order_relaxed);
    pendingLv2Path[(size_t) slotIdx].clear();
    pendingLv2PluginId[(size_t) slotIdx].clear();
    pendingLv2State[(size_t) slotIdx].clear();
}

void AuxLaneStrip::setPendingNativeLv2 (int slotIdx, const juce::File& path,
                                         std::vector<uint8_t> state,
                                         const juce::String& pluginId,
                                         const juce::File& stateDir) noexcept
{
    jassert (slotIdx >= 0 && slotIdx < kMaxPlugins);
    pendingLv2Path[(size_t) slotIdx]     = path.getFullPathName();
    pendingLv2PluginId[(size_t) slotIdx] = pluginId;
    pendingLv2State[(size_t) slotIdx]    = std::move (state);
    pendingLv2StateDir[(size_t) slotIdx] = stateDir;
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_VST3
bool AuxLaneStrip::loadNativeVst3 (int slotIdx, const juce::File& path, std::string& errorOut,
                                   const juce::String& pluginId)
{
    jassert (slotIdx >= 0 && slotIdx < kMaxPlugins);
    if (preparedSampleRate <= 0.0 || preparedBlockSize <= 0)
    { errorOut = "aux lane not prepared"; return false; }
    // One host per slot — see loadNativeClap.
    unloadNativeClap (slotIdx);
    unloadNativeLv2 (slotIdx);
    slots[(size_t) slotIdx].unload();
    const bool ok = nativeVst3Slots[(size_t) slotIdx].load (path, preparedSampleRate, preparedBlockSize, errorOut, pluginId);
    vst3ReloadFailed[(size_t) slotIdx].store (false, std::memory_order_relaxed);
    return ok;
}

void AuxLaneStrip::unloadNativeVst3 (int slotIdx) noexcept
{
    jassert (slotIdx >= 0 && slotIdx < kMaxPlugins);
    nativeVst3Slots[(size_t) slotIdx].unload();
    vst3ReloadFailed[(size_t) slotIdx].store (false, std::memory_order_relaxed);
    pendingVst3Path[(size_t) slotIdx].clear();
    pendingVst3PluginId[(size_t) slotIdx].clear();
    pendingVst3State[(size_t) slotIdx].clear();
}

void AuxLaneStrip::setPendingNativeVst3 (int slotIdx, const juce::File& path,
                                          std::vector<uint8_t> state,
                                          const juce::String& pluginId) noexcept
{
    jassert (slotIdx >= 0 && slotIdx < kMaxPlugins);
    pendingVst3Path[(size_t) slotIdx]     = path.getFullPathName();
    pendingVst3PluginId[(size_t) slotIdx] = pluginId;
    pendingVst3State[(size_t) slotIdx]    = std::move (state);
}
#endif

void AuxLaneStrip::updateGainTarget() noexcept
{
    if (paramsRef == nullptr) return;
    const float db = paramsRef->liveReturnLevelDb.load (std::memory_order_relaxed);
    // Same -inf-via-sentinel pattern as the channel strip's fader: anything
    // at or below the floor hard-mutes (avoids feeding reverb tails through
    // an inaudible-but-nonzero gain).
    const float gain = (db <= ChannelStripParams::kFaderInfThreshDb)
                         ? 0.0f
                         : juce::Decibels::decibelsToGain (db);
    returnGain.setTargetValue (gain);
}

void AuxLaneStrip::processStereoBlock (float* L, float* R, int numSamples,
                                          const float* const* deviceInputs,
                                          int   numDeviceInputs,
                                          float* const*       deviceOutputs,
                                          int   numDeviceOutputs) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    if (numSamples == 0) return;
    if (paramsRef == nullptr) return;

    // Oversized-block bail. insertScratchL/R are sized to prepare()'s
    // blockSize and the audio thread refuses to allocate, so a host that
    // hands us numSamples > scratch capacity must skip the whole block —
    // partial processing would desync the meter / smoother bookkeeping
    // between the slot loop and the return-gain pass below. L/R is the
    // lane accumulator with channel-strip sends already summed in, so
    // we MUST clear it (otherwise the unprocessed sum leaks into master
    // un-EQ'd / un-comp'd) and drop the meter to -inf so the UI doesn't
    // hold a stale reading from the previous block.
    jassert (numSamples <= (int) insertScratchL.size());
    if (numSamples > (int) insertScratchL.size())
    {
        std::memset (L, 0, sizeof (float) * (size_t) numSamples);
        std::memset (R, 0, sizeof (float) * (size_t) numSamples);
        paramsRef->meterPostL.store (-100.0f, std::memory_order_relaxed);
        paramsRef->meterPostR.store (-100.0f, std::memory_order_relaxed);
        return;
    }

    // Mute path: clear in-place so the engine's accumulator into master
    // sees silence (the lane's buffer is reused across blocks). Reads
    // liveMute (post-automation) so automated mutes also drop the lane.
    if (paramsRef->liveMute.load (std::memory_order_relaxed))
    {
        std::memset (L, 0, sizeof (float) * (size_t) numSamples);
        std::memset (R, 0, sizeof (float) * (size_t) numSamples);
        paramsRef->meterPostL.store (-100.0f, std::memory_order_relaxed);
        paramsRef->meterPostR.store (-100.0f, std::memory_order_relaxed);
        return;
    }

    updateGainTarget();

    pluginMidiScratch.clear();

    // Each slot can be in plugin / hardware / empty mode independently.
    // The dispatcher runs whichever is active, crossfades to silence on
    // a mode flip, then ramps back up after the swap completes.
    for (int s = 0; s < kMaxPlugins; ++s)
    {
        const auto sIdx = (size_t) s;
        const int req = insertMode[sIdx].load (std::memory_order_acquire);
        if (req != activeInsertMode[sIdx])
        {
            if (activeInsertGain[sIdx].getCurrentValue() > 1.0e-4f)
            {
                activeInsertGain[sIdx].setTargetValue (0.0f);
            }
            else
            {
                activeInsertMode[sIdx] = req;
                if (activeInsertMode[sIdx] == kInsertHardware)
                    hardwareSlots[sIdx].resetTailsAndDelayLine();
                activeInsertGain[sIdx].setTargetValue (
                    activeInsertMode[sIdx] == kInsertEmpty ? 0.0f : 1.0f);
            }
        }
        else
        {
            activeInsertGain[sIdx].setTargetValue (
                activeInsertMode[sIdx] == kInsertEmpty ? 0.0f : 1.0f);
        }

        // Pre-insert stash for the crossfade gate.
        std::memcpy (insertScratchL.data(), L, sizeof (float) * (size_t) numSamples);
        std::memcpy (insertScratchR.data(), R, sizeof (float) * (size_t) numSamples);

        if (activeInsertMode[sIdx] == kInsertPlugin)
        {
            // Native CLAP host takes the slot when loaded; otherwise the JUCE
            // PluginSlot. processStereo is in-place safe (own scratch buffers).
#if DUSKSTUDIO_HAS_NATIVE_CLAP
            if (nativeClapSlots[sIdx].isLoaded())
                nativeClapSlots[sIdx].processStereo (L, R, L, R, numSamples);
            else
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
            if (nativeLv2Slots[sIdx].isLoaded())
                nativeLv2Slots[sIdx].processStereo (L, R, L, R, numSamples);
            else
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
            if (nativeVst3Slots[sIdx].isLoaded())
                nativeVst3Slots[sIdx].processStereo (L, R, L, R, numSamples);
            else
#endif
                slots[sIdx].processStereoBlock (L, R, numSamples, pluginMidiScratch);
        }
        else if (activeInsertMode[sIdx] == kInsertHardware)
        {
            hardwareSlots[sIdx].processStereoBlock (L, R, numSamples,
                                                     deviceInputs, numDeviceInputs,
                                                     deviceOutputs, numDeviceOutputs);
        }

        // Blend pre vs post by the gate. An empty slot collapses to pre
        // entirely once the gate has ramped down.
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = activeInsertGain[sIdx].getNextValue();
            L[i] = (1.0f - g) * insertScratchL[(size_t) i] + g * L[i];
            R[i] = (1.0f - g) * insertScratchR[(size_t) i] + g * R[i];
        }
    }

    // Cue / headphone tap. Send the processed aux mix to its routed output
    // pair BEFORE the return fader/mute below, so the hardware pair is an
    // independent cue send (the fader/mute govern only the fold into master).
    outputpair::tapStereoPairInto (deviceOutputs, numDeviceOutputs, L, R, numSamples,
                                     paramsRef->outputPair.load (std::memory_order_relaxed));

    // Return level + meter peak.
    float postPeakL = 0.0f, postPeakR = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float g = returnGain.getNextValue();
        L[i] *= g;
        R[i] *= g;
        const float aL = std::fabs (L[i]);
        const float aR = std::fabs (R[i]);
        if (aL > postPeakL) postPeakL = aL;
        if (aR > postPeakR) postPeakR = aR;
    }

    const auto toDb = [] (float a)
    {
        return a > 1.0e-5f ? juce::Decibels::gainToDecibels (a, -100.0f) : -100.0f;
    };
    paramsRef->meterPostL.store (toDb (postPeakL), std::memory_order_relaxed);
    paramsRef->meterPostR.store (toDb (postPeakR), std::memory_order_relaxed);
}
} // namespace duskstudio

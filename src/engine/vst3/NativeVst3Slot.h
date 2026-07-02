#pragma once

#include "Vst3Bundle.h"
#include "Vst3Instance.h"
#include "../hosting/InsertAdapter.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::vst3
{
// One insert slot backed by the native VST3 host: owns the module + the instance,
// loads / prepares / processes / unloads a VST3 plugin, and exposes the live
// instance so the UI can attach an editor. Same public contract as
// NativeClapSlot / NativeLv2Slot, so the DSP call sites and session code treat
// all three formats alike.
//
// Concurrency: a load / unload while the audio thread might be mid-process must be
// fenced by the engine's process gate (suspend → silent buffers → swap → resume);
// the slot itself only guards the steady state via the acquire/release `ready` flag.
class NativeVst3Slot
{
public:
    NativeVst3Slot() = default;

    // Message thread: load the module at `path` and instantiate its first audio
    // effect class, replacing any prior load. False (+ errorOut) on failure.
    bool load (const juce::File& path, double sampleRate, int maxBlock, std::string& errorOut);
    void unload();

    // Re-activate the loaded instance at a new sample-rate / block-size. VST3
    // renegotiates through setupProcessing on the SAME instance, so state and an
    // open editor survive. No-op (false) when nothing is loaded. Caller fences
    // the audio thread.
    bool reactivate (double sampleRate, int maxBlock, std::string& errorOut);

    // App shutdown: release the instance + module WITHOUT destroying, matching
    // the CLAP/LV2 paths — teardown order across the plugin .so is skipped on
    // the way out (the OS reclaims the memory as the process exits).
    void leakForShutdown() noexcept;

    bool isLoaded() const noexcept { return ready.load (std::memory_order_acquire); }
    const juce::String& getPath() const noexcept { return loadedPath; }

    void setBypassed (bool b) noexcept { bypassed.store (b, std::memory_order_relaxed); }
    bool isBypassed()   const noexcept { return bypassed.load (std::memory_order_relaxed); }

    bool saveState (std::vector<uint8_t>& out) const;
    bool loadState (const std::vector<uint8_t>& in);

    // Audio thread: process stereo through the plugin, via the InsertAdapter →
    // INativeInstance::processBlock. Clears the outputs + returns when no plugin
    // is loaded; passes audio through when bypassed.
    void processStereo (const float* inL, const float* inR,
                        float* outL, float* outR, int numFrames) noexcept;

    // UI: the live instance for editor attach (nullptr when not loaded).
    Vst3Instance* getInstance() noexcept
        { return ready.load (std::memory_order_acquire) ? instance.get() : nullptr; }

private:
    // bundle declared first so it is destroyed LAST — the instance's vtables live
    // in the module the bundle keeps resident.
    std::unique_ptr<Vst3Bundle>   bundle;
    std::unique_ptr<Vst3Instance> instance;
    hosting::InsertAdapter        adapter;
    std::atomic<bool> ready    { false };
    std::atomic<bool> bypassed { false };
    juce::String      loadedPath;
};
} // namespace duskstudio::vst3

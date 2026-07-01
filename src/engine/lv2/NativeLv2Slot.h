#pragma once

#include "Lv2Bundle.h"
#include "Lv2Instance.h"
#include "../hosting/InsertAdapter.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::lv2
{
// One insert slot backed by the native LV2 host: owns the bundle + the instance,
// loads / prepares / processes / unloads an LV2 plugin, and exposes the live
// instance so the UI can attach an editor. load / unload are message-thread;
// processStereo is the audio path and reads a lock-free `ready` flag so it no-ops
// cleanly when empty. Direct mirror of NativeClapSlot — same public contract, so
// the DSP call sites and session code treat CLAP and LV2 alike.
//
// Concurrency: a load / unload while the audio thread might be mid-process must be
// fenced by the engine's process gate (suspend → silent buffers → swap → resume);
// the slot itself only guards the steady state via the acquire/release `ready` flag.
class NativeLv2Slot
{
public:
    NativeLv2Slot() = default;

    // Message thread: load + create + activate the first audio effect in the .lv2
    // bundle at `path`, replacing any prior load. False (+ errorOut) on failure.
    bool load (const juce::File& path, double sampleRate, int maxBlock, std::string& errorOut);
    void unload();

    // Re-activate the already-loaded instance at a new sample-rate / block-size.
    // LV2 fixes the rate at instantiate, so this re-instantiates the plugin; its
    // control-port values carry across (state-extension blobs aren't wired yet).
    // No-op (false) when nothing is loaded. Caller fences the audio thread.
    bool reactivate (double sampleRate, int maxBlock, std::string& errorOut);

    // App shutdown: release the instance + bundle WITHOUT destroying, matching the
    // CLAP path — teardown order across lilv + the plugin .so is skipped entirely
    // on the way out (the OS reclaims the memory as the process exits).
    void leakForShutdown() noexcept;

    bool isLoaded() const noexcept { return ready.load (std::memory_order_acquire); }
    const juce::String& getPath() const noexcept { return loadedPath; }

    void setBypassed (bool b) noexcept { bypassed.store (b, std::memory_order_relaxed); }
    bool isBypassed()   const noexcept { return bypassed.load (std::memory_order_relaxed); }

    bool saveState (std::vector<uint8_t>& out) const;
    bool loadState (const std::vector<uint8_t>& in);

    // Audio thread: process stereo through the plugin, via the InsertAdapter →
    // INativeInstance::processBlock. Clears the outputs + returns when no plugin is
    // loaded; passes audio through when bypassed.
    void processStereo (const float* inL, const float* inR,
                        float* outL, float* outR, int numFrames) noexcept;

    // UI: the live instance for editor attach (nullptr when not loaded).
    Lv2Instance* getInstance() noexcept
        { return ready.load (std::memory_order_acquire) ? instance.get() : nullptr; }

private:
    // bundle declared first so it is destroyed LAST — the instance (whose LilvPlugin
    // points into the bundle's world) must tear down before the world is freed.
    std::unique_ptr<Lv2Bundle>   bundle;
    std::unique_ptr<Lv2Instance> instance;
    hosting::InsertAdapter       adapter;
    std::atomic<bool> ready    { false };
    std::atomic<bool> bypassed { false };
    juce::String      loadedPath;
};
} // namespace duskstudio::lv2

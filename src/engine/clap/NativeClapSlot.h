#pragma once

#include "ClapBundle.h"
#include "ClapInstance.h"
#include "../hosting/InsertAdapter.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::clap
{
// One aux-lane plugin slot backed by the native CLAP host: owns the bundle + the
// instance, loads / prepares / processes / unloads a .clap, and exposes the live
// instance so the UI can attach a native ClapEditor that shares it (one instance
// for audio + editor). load / unload are message-thread; processStereo is the
// audio path and reads a lock-free `ready` flag so it no-ops cleanly when empty.
//
// Concurrency: a load / unload while the audio thread might be mid-process must be
// fenced by the engine's process gate (suspend → silent buffers → swap → resume),
// per [[project_multicore_dsp]] — the slot itself only guards the steady state via
// the acquire/release `ready` flag.
class NativeClapSlot
{
public:
    NativeClapSlot() = default;

    // Message thread: load + create + activate the first plugin in the .clap at
    // `path`, replacing any prior load. False (+ errorOut) on failure.
    bool load (const juce::File& path, double sampleRate, int maxBlock, std::string& errorOut);
    void unload();

    // Re-activate the already-loaded instance at a new sample-rate / block-size WITHOUT
    // tearing it down — used on a device-rate or oversampling-factor change so an open
    // editor and the plugin's state survive. No-op (false) when nothing is loaded. Caller
    // fences the audio thread via the engine process gate.
    bool reactivate (double sampleRate, int maxBlock, std::string& errorOut);

    // App shutdown: release the instance + bundle WITHOUT destroying (u-he plugins
    // hang in deactivate/destroy/dlclose). The process is exiting; the OS reclaims it.
    void leakForShutdown() noexcept;

    bool isLoaded() const noexcept { return ready.load (std::memory_order_acquire); }
    const juce::String& getPath() const noexcept { return loadedPath; }

    // Bypass: processStereo passes audio through untouched (the plugin still loaded).
    void setBypassed (bool b) noexcept { bypassed.store (b, std::memory_order_relaxed); }
    bool isBypassed()   const noexcept { return bypassed.load (std::memory_order_relaxed); }

    // Message thread: serialise / restore the loaded plugin's state for session
    // persistence. No-op (false) when nothing is loaded.
    bool saveState (std::vector<uint8_t>& out) const;
    bool loadState (const std::vector<uint8_t>& in);

    // Parameters (message thread for read/enumerate; setParamValue is the control
    // entry — queued and applied on the next audio block). No-op / empty when unloaded.
    int paramCount() const noexcept { return instance != nullptr ? instance->paramCount() : 0; }
    const ClapInstance::ParamInfo* paramInfo (int index) const noexcept
        { return instance != nullptr ? instance->paramInfo (index) : nullptr; }
    bool getParamValue (clap_id id, double& out) const
        { return instance != nullptr && instance->getParamValue (id, out); }
    void setParamValue (clap_id id, double value)
        { if (instance != nullptr) instance->setParamValue (id, value); }

    // Audio thread: process stereo through the plugin, via the InsertAdapter →
    // INativeInstance::processBlock (the generalized host path). Clears the outputs
    // + returns when no plugin is loaded; passes audio through when bypassed.
    void processStereo (const float* inL, const float* inR,
                        float* outL, float* outR, int numFrames) noexcept;

    // UI: the live instance for editor attach (nullptr when not loaded).
    ClapInstance* getInstance() noexcept
        { return ready.load (std::memory_order_acquire) ? instance.get() : nullptr; }

private:
    // bundle declared first so it is destroyed LAST — the instance (and its plugin
    // vtable, which lives in the bundle's .so) must tear down before the .so unloads.
    std::unique_ptr<ClapBundle>   bundle;
    std::unique_ptr<ClapInstance> instance;
    // Folds the mixer's stereo insert onto the plugin's negotiated layout. Prepared
    // on load / reactivate; owns its own scratch (no plugin refs, teardown order-free).
    hosting::InsertAdapter        adapter;
    std::atomic<bool> ready    { false };
    std::atomic<bool> bypassed { false };
    juce::String      loadedPath;
};
} // namespace duskstudio::clap

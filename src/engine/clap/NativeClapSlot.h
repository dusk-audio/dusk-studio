#pragma once

#include "ClapBundle.h"
#include "ClapInstance.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <string>

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

    bool isLoaded() const noexcept { return ready.load (std::memory_order_acquire); }
    const juce::String& getPath() const noexcept { return loadedPath; }

    // Audio thread: process stereo through the plugin; clears the outputs + returns
    // when no plugin is loaded.
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
    std::atomic<bool> ready { false };
    juce::String      loadedPath;
};
} // namespace duskstudio::clap

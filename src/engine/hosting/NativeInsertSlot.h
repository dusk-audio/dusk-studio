#pragma once

#include "InsertAdapter.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::hosting
{
// One insert slot backed by a native plugin host: owns the bundle + the instance,
// loads / prepares / processes / unloads a plugin, and exposes the live instance
// so the UI can attach a native editor that shares it (one instance for audio +
// editor). Traits bind a format:
//
//   using Bundle / Instance;                  // Instance implements INativeInstance
//   static constexpr const char* bundleNoun;  // load-error prefix ("bundle"/"module")
//   static bool pickPlugin (const Bundle&, std::string& idOut, std::string& errorOut);
//
// load / unload / state are message-thread; processStereo is the audio path and
// reads a lock-free `ready` flag so it no-ops cleanly when empty.
//
// Concurrency: a load / unload while the audio thread might be mid-process must be
// fenced by the engine's process gate (suspend → silent buffers → swap → resume);
// the slot itself only guards the steady state via the acquire/release `ready` flag.
template <typename Traits>
class NativeInsertSlot
{
public:
    using Bundle   = typename Traits::Bundle;
    using Instance = typename Traits::Instance;

    NativeInsertSlot() = default;

    // Message thread: load the bundle at `path` and instantiate the plugin the
    // Traits pick, replacing any prior load. False (+ errorOut) on failure.
    bool load (const juce::File& path, double sampleRate, int maxBlock, std::string& errorOut)
    {
        unload();

        auto b = std::make_unique<Bundle>();
        std::string err;
        if (! b->load (path.getFullPathName().toStdString(), err))
        { errorOut = std::string (Traits::bundleNoun) + ": " + err; return false; }

        std::string pluginId;
        if (! Traits::pickPlugin (*b, pluginId, errorOut))
            return false;

        auto inst = std::make_unique<Instance>();
        if (! inst->create (*b, pluginId, err))
        { errorOut = "create: " + err; return false; }
        if (! inst->activate (sampleRate, maxBlock, err))
        { errorOut = "activate: " + err; return false; }

        // Publish only after both are fully built; the audio thread's acquire-load
        // of `ready` pairs with this release so it never sees a half-built instance.
        bundle     = std::move (b);
        instance   = std::move (inst);
        adapter.prepare (instance->portLayout(), maxBlock);   // size the fold scratch
        loadedPath = path.getFullPathName();
        ready.store (true, std::memory_order_release);
        return true;
    }

    void unload()
    {
        ready.store (false, std::memory_order_release);
        instance.reset();   // destroy the plugin first — its vtables live in the bundle
        bundle.reset();     // then unload the .so / world backing them
        loadedPath.clear();
    }

    // Re-activate the loaded instance at a new sample-rate / block-size WITHOUT a
    // full reload, so an open editor and the plugin's state survive (formats whose
    // instance can't renegotiate in place re-instantiate internally and carry the
    // state across). No-op (false) when nothing is loaded. Caller fences the audio
    // thread via the engine process gate.
    bool reactivate (double sampleRate, int maxBlock, std::string& errorOut)
    {
        if (instance == nullptr) { errorOut = "no instance"; return false; }
        // `ready`/`instance` are unchanged across the call; the audio thread (fenced
        // by the engine gate) sees active==false mid-swap and no-ops, then resumes.
        if (! instance->reactivate (sampleRate, maxBlock, errorOut)) return false;
        adapter.prepare (instance->portLayout(), maxBlock);   // block size may have changed
        return true;
    }

    // App shutdown: release the instance + bundle WITHOUT destroying (u-he plugins
    // hang in deactivate/destroy/dlclose). The process is exiting; the OS reclaims it.
    void leakForShutdown() noexcept
    {
        ready.store (false, std::memory_order_release);
        (void) instance.release();
        (void) bundle.release();
        loadedPath.clear();
    }

    bool isLoaded() const noexcept { return ready.load (std::memory_order_acquire); }
    const juce::String& getPath() const noexcept { return loadedPath; }

    // Bypass: processStereo passes audio through untouched (the plugin stays loaded).
    void setBypassed (bool b) noexcept { bypassed.store (b, std::memory_order_relaxed); }
    bool isBypassed()   const noexcept { return bypassed.load (std::memory_order_relaxed); }

    // Message thread: serialise / restore the loaded plugin's state for session
    // persistence. No-op (false) when nothing is loaded.
    bool saveState (std::vector<uint8_t>& out) const
    { return instance != nullptr && instance->saveState (out); }
    bool loadState (const std::vector<uint8_t>& in)
    { return instance != nullptr && instance->loadState (in); }

    // Audio thread: process stereo through the plugin, via the InsertAdapter →
    // INativeInstance::processBlock. Clears the outputs + returns when no plugin
    // is loaded; passes audio through when bypassed.
    void processStereo (const float* inL, const float* inR,
                        float* outL, float* outR, int numFrames) noexcept
    {
        auto clearOutputs = [&]
        {
            if (numFrames <= 0) return;
            const size_t n = sizeof (float) * (size_t) numFrames;
            if (outL != nullptr) std::memset (outL, 0, n);
            if (outR != nullptr) std::memset (outR, 0, n);
        };

        // Clear when empty or momentarily inactive (mid-reactivate, fenced by the
        // engine gate) — a no-op silent pass.
        if (! ready.load (std::memory_order_acquire) || instance == nullptr
            || ! instance->isActive() || inL == nullptr)
        {
            clearOutputs();
            return;
        }

        if (numFrames <= 0) return;
        const size_t n = sizeof (float) * (size_t) numFrames;

        if (bypassed.load (std::memory_order_relaxed))
        {
            // Passthrough — copy in→out (a no-op when called in-place).
            if (outL != nullptr && outL != inL) std::memcpy (outL, inL, n);
            const float* r = (inR != nullptr) ? inR : inL;
            if (outR != nullptr && outR != r)   std::memcpy (outR, r, n);
            return;
        }

        // The adapter needs both output buffers to process in place; a missing one
        // is the same silent no-op as an empty slot.
        if (outL == nullptr || outR == nullptr)
        {
            clearOutputs();
            return;
        }

        // The InsertAdapter processes in place, so stage the input into the output
        // buffers first (a no-op for the in-place call sites where out == in).
        if (outL != inL) std::memcpy (outL, inL, n);
        const float* r = (inR != nullptr) ? inR : inL;
        if (outR != r)  std::memcpy (outR, r, n);

        adapter.process (*instance, outL, outR, numFrames);
    }

    // UI: the live instance for editor attach (nullptr when not loaded).
    Instance* getInstance() noexcept
        { return ready.load (std::memory_order_acquire) ? instance.get() : nullptr; }

protected:
    // bundle declared first so it is destroyed LAST — the instance's vtables live
    // in the bundle's .so / world.
    std::unique_ptr<Bundle>   bundle;
    std::unique_ptr<Instance> instance;
    // Folds the mixer's stereo insert onto the plugin's negotiated layout. Prepared
    // on load / reactivate; owns its own scratch (no plugin refs, teardown order-free).
    InsertAdapter     adapter;
    std::atomic<bool> ready    { false };
    std::atomic<bool> bypassed { false };
    juce::String      loadedPath;
};
} // namespace duskstudio::hosting

#pragma once

#include "DuskMultisampleProcessor.h"
#include "MultisampleBundle.h"
#include "../hosting/NativeInsertSlot.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace duskstudio
{
struct MultisampleSlotTraits
{
    using Bundle   = MultisampleBundle;
    using Instance = DuskMultisampleProcessor;
    static constexpr const char* bundleNoun = "soundfont";

    // One instrument per soundfont - there is no id to pick.
    static bool pickPlugin (const MultisampleBundle&, const std::string&,
                            std::string& idOut, std::string&)
    {
        idOut.clear();
        return true;
    }
};

class NativeMultisampleSlot final : public hosting::NativeInsertSlot<MultisampleSlotTraits>
{
public:
    // The retained soundfont reference used by session persistence and Reload.
    // It can name the last successful file while the runtime is silent after a
    // failed load. The editor swaps files in place without going through load(),
    // so the slot's bundle path goes stale and must never be used as a fallback.
    std::string getLoadedSoundfontPath() const
    {
        return instance != nullptr ? instance->getLoadedPathSnapshot() : std::string();
    }

    // Message thread. Join any in-flight background load so the caller can do it
    // BEFORE parking the audio thread on an unload (see cancelPendingLoads).
    void drainPendingLoads()
    {
        if (instance != nullptr) instance->cancelPendingLoads();
    }

    void prepareReplacement()
    {
        if (instance != nullptr) instance->drainPendingLoadsForReplacement();
        releaseAttachedEditor();
    }

    // State's retained soundfont reference is authoritative. Keep the generic
    // slot path in step when an in-editor file swap made it differ from the
    // bundle that originally constructed the slot.
    bool loadState (const std::vector<uint8_t>& in)
    {
        if (instance == nullptr || ! instance->loadState (in))
            return false;
        const auto retainedPath = instance->getLoadedPathSnapshot();
        if (! retainedPath.empty())
            loadedPath = retainedPath;
        return true;
    }

    // Two-phase load, for callers with a live engine. Parsing a soundfont takes
    // seconds on a GM bank, so it must not happen inside the engine's process
    // gate: prime() builds + activates + parses + restores state with the audio
    // thread running the OLD occupant, and commit() does the only part that has
    // to be fenced - the swap. Same prime-then-atomic-swap shape as
    // PluginSlot::loadFromFile. The inherited single-shot load() stays for
    // callers with no running engine (harness paths, pending session restore).
    struct PrimedLoad
    {
        std::unique_ptr<MultisampleBundle>        bundle;
        std::unique_ptr<DuskMultisampleProcessor> instance;
        hosting::InsertAdapter                    adapter;
        std::string                               path;
        int                                       preparedMaxBlock = 0;
        bool                                      stateRestoreFailed = false;

        explicit operator bool() const noexcept { return instance != nullptr; }
    };

    // Message thread, NO gate. Empty result (+ errorOut) on failure.
    static PrimedLoad prime (const std::filesystem::path& path,
                             double sampleRate, int maxBlock,
                             std::string& errorOut,
                             const std::vector<std::uint8_t>* state = nullptr,
                             bool resolvedPathAuthoritative = false)
    {
        PrimedLoad primed;
        auto b = std::make_unique<MultisampleBundle>();
        std::string err;
        if (! b->load (path.u8string(), err))
        {
            errorOut = std::string (MultisampleSlotTraits::bundleNoun) + ": " + err;
            return primed;
        }

        auto inst = std::make_unique<DuskMultisampleProcessor>();
        const bool hasState = state != nullptr && ! state->empty();
        if (hasState && ! (resolvedPathAuthoritative
                            ? inst->loadStateWithResolvedFile (*state, path)
                            : inst->loadState (*state)))
        {
            // Carry the failure to the caller. The caller deliberately does
            // not commit this primed default-state instance, so the saved blob
            // remains authoritative and the session slot stays offline.
            primed.stateRestoreFailed = true;
            errorOut = "state restore failed";
        }
        const bool stateRuntimeLoaded = inst->hasLoadedRuntime();
        if (! stateRuntimeLoaded)
        {
            // The state loader already applied non-file settings. When its file
            // was unavailable, keep that diagnostic while loading the bundle
            // instead of parsing the failed state file a second time.
            if (! inst->create (*b, {}, err, hasState))
            { errorOut = "create: " + err; return primed; }
        }
        if (! inst->activate (sampleRate, maxBlock, err))
        { errorOut = "activate: " + err; return primed; }
        primed.adapter.prepare (inst->portLayout(), maxBlock);
        primed.preparedMaxBlock = maxBlock;

        primed.bundle   = std::move (b);
        primed.instance = std::move (inst);
        primed.path     = primed.instance->getLoadedPathSnapshot();
        return primed;
    }

    // Fenced swap only. primed retains the outgoing occupant and scratch; the
    // caller destroys it after leaving the engine gate.
    bool commit (PrimedLoad& primed, int maxBlock)
    {
        if (! primed || primed.stateRestoreFailed || primed.preparedMaxBlock != maxBlock)
            return false;
        releaseAttachedEditor();
        processingOnline.store (false, std::memory_order_release);
        ready.store (false, std::memory_order_release);
        gen.fetch_add (1, std::memory_order_relaxed);
        drainQueuedParamBindings();
        owesMidiReset = false;
        owedResetMidi.clear();
        bundle.swap (primed.bundle);
        instance.swap (primed.instance);
        std::swap (adapter, primed.adapter);
        loadedPath.swap (primed.path);
        loadedPluginId.clear();
        processingOnline.store (true, std::memory_order_release);
        ready.store (true, std::memory_order_release);
        return true;
    }

    // Fenced removal with the same deferred destruction contract as commit.
    PrimedLoad detach()
    {
        releaseAttachedEditor();
        processingOnline.store (false, std::memory_order_release);
        ready.store (false, std::memory_order_release);
        gen.fetch_add (1, std::memory_order_relaxed);
        drainQueuedParamBindings();
        owesMidiReset = false;
        owedResetMidi.clear();
        PrimedLoad outgoing;
        bundle.swap (outgoing.bundle);
        instance.swap (outgoing.instance);
        std::swap (adapter, outgoing.adapter);
        loadedPath.swap (outgoing.path);
        loadedPluginId.clear();
        return outgoing;
    }
};
} // namespace duskstudio

#pragma once

#include "DuskMultisampleProcessor.h"
#include "MultisampleBundle.h"
#include "../hosting/NativeInsertSlot.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
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
    // The soundfont playing RIGHT NOW, empty when none is. The editor swaps
    // files in place (Browse / Reload / SF2 preset switch / Clear) without going
    // through load(), so the slot's own bundle path goes stale - never fall back
    // to it, or a save would resurrect a soundfont the user cleared.
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
        std::string                               path;

        explicit operator bool() const noexcept { return instance != nullptr; }
    };

    // Message thread, NO gate. Empty result (+ errorOut) on failure.
    static PrimedLoad prime (const std::filesystem::path& path,
                             double sampleRate, int maxBlock,
                             std::string& errorOut,
                             const std::vector<std::uint8_t>* state = nullptr)
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
        if (! inst->create (*b, {}, err)) { errorOut = "create: " + err;   return primed; }
        if (! inst->activate (sampleRate, maxBlock, err))
        { errorOut = "activate: " + err; return primed; }
        // State carries the SF2 preset index, which can trigger a second parse -
        // also expensive, so it belongs on this side of the gate.
        if (state != nullptr && ! state->empty()) inst->loadState (*state);

        primed.bundle   = std::move (b);
        primed.instance = std::move (inst);
        primed.path     = path.u8string();
        return primed;
    }

    // Message thread, audio thread fenced by the caller. Installs a primed
    // instance and destroys the previous one; a failed prime is a no-op false.
    // Drain the OUTGOING instance's loader before calling (drainPendingLoads) so
    // its destructor does not join a decode while the audio thread is parked.
    bool commit (PrimedLoad primed, int maxBlock)
    {
        if (! primed) return false;
        unload();
        bundle   = std::move (primed.bundle);
        instance = std::move (primed.instance);
        adapter.prepare (instance->portLayout(), maxBlock);
        loadedPath = std::move (primed.path);
        loadedPluginId.clear();
        ready.store (true, std::memory_order_release);
        return true;
    }
};
} // namespace duskstudio

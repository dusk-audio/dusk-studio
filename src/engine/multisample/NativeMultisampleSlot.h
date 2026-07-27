#pragma once

#include "DuskMultisampleProcessor.h"
#include "MultisampleBundle.h"
#include "../hosting/NativeInsertSlot.h"

#include <string>

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
};
} // namespace duskstudio

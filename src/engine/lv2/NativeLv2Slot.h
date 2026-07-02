#pragma once

#include "Lv2Bundle.h"
#include "Lv2Instance.h"
#include "../hosting/NativeInsertSlot.h"

namespace duskstudio::lv2
{
struct Lv2SlotTraits
{
    using Bundle   = Lv2Bundle;
    using Instance = Lv2Instance;
    static constexpr const char* bundleNoun = "bundle";

    static bool pickPlugin (const Lv2Bundle& b, std::string& idOut, std::string& errorOut)
    {
        if (b.plugins().empty())
        { errorOut = "no plugins in bundle"; return false; }
        // First audio effect (audio in + out); fall back to the first advertised plugin.
        idOut = b.plugins().front().uri;
        for (const auto& d : b.plugins())
            if (d.audioInputs > 0 && d.audioOutputs > 0) { idOut = d.uri; break; }
        return true;
    }
};

class NativeLv2Slot final : public hosting::NativeInsertSlot<Lv2SlotTraits> {};
} // namespace duskstudio::lv2

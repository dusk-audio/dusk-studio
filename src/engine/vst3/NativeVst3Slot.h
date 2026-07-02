#pragma once

#include "Vst3Bundle.h"
#include "Vst3Instance.h"
#include "../hosting/NativeInsertSlot.h"

namespace duskstudio::vst3
{
struct Vst3SlotTraits
{
    using Bundle   = Vst3Bundle;
    using Instance = Vst3Instance;
    static constexpr const char* bundleNoun = "module";

    static bool pickPlugin (const Vst3Bundle& b, std::string& idOut, std::string& errorOut)
    {
        // First audio effect class; instruments need a source, not an insert.
        for (const auto& d : b.plugins())
            if (! d.isInstrument) { idOut = d.id; return true; }
        errorOut = "no effect class in module";
        return false;
    }
};

class NativeVst3Slot final : public hosting::NativeInsertSlot<Vst3SlotTraits> {};
} // namespace duskstudio::vst3

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

// LV2 insert slot — the shared NativeInsertSlot plus parameter forwarding
// (input control ports, values in the port's own units; mirrors NativeClapSlot).
class NativeLv2Slot final : public hosting::NativeInsertSlot<Lv2SlotTraits>
{
public:
    // Parameters (message thread for read/enumerate; setParamValue is the control
    // entry — staged and applied on the next audio block). No-op / empty when unloaded.
    int paramCount() const noexcept { return instance != nullptr ? instance->paramCount() : 0; }
    const Lv2Instance::ParamInfo* paramInfo (int index) const noexcept
        { return instance != nullptr ? instance->paramInfo (index) : nullptr; }
    bool getParamValue (uint32_t portIndex, double& out) const
        { return instance != nullptr && instance->getParamValue (portIndex, out); }
    void setParamValue (uint32_t portIndex, double value)
        { if (instance != nullptr) instance->setParamValue (portIndex, value); }

    // MIDI Learn: the plugin-GUI knob the user moved last (-1 = none).
    int lastTouchedParamIndex() const noexcept
        { return instance != nullptr ? instance->lastTouchedParamIndex() : -1; }

protected:
    // MIDI binding: 0..1 fraction → the port's own min..max range.
    void applyParamBinding (uint32_t paramIndex, float frac) override
    {
        const auto* p = paramInfo ((int) paramIndex);
        if (p == nullptr) return;
        setParamValue (p->id, (double) p->minValue
                                + (double) frac * ((double) p->maxValue - (double) p->minValue));
    }
};
} // namespace duskstudio::lv2

#pragma once

#include "ClapBundle.h"
#include "ClapInstance.h"
#include "../hosting/NativeInsertSlot.h"

namespace duskstudio::clap
{
struct ClapSlotTraits
{
    using Bundle   = ClapBundle;
    using Instance = ClapInstance;
    static constexpr const char* bundleNoun = "bundle";

    static bool pickPlugin (const ClapBundle& b, std::string& idOut, std::string& errorOut)
    {
        if (b.plugins().empty())
        { errorOut = "no plugins in bundle"; return false; }
        idOut = b.plugins().front().id;
        return true;
    }
};

// CLAP insert slot — the shared NativeInsertSlot plus parameter forwarding, which
// stays CLAP-only until the other native hosts grow a parameter surface.
class NativeClapSlot final : public hosting::NativeInsertSlot<ClapSlotTraits>
{
public:
    // Parameters (message thread for read/enumerate; setParamValue is the control
    // entry — queued and applied on the next audio block). No-op / empty when unloaded.
    int paramCount() const noexcept { return instance != nullptr ? instance->paramCount() : 0; }
    const ClapInstance::ParamInfo* paramInfo (int index) const noexcept
        { return instance != nullptr ? instance->paramInfo (index) : nullptr; }
    bool getParamValue (clap_id id, double& out) const
        { return instance != nullptr && instance->getParamValue (id, out); }
    void setParamValue (clap_id id, double value)
        { if (instance != nullptr) instance->setParamValue (id, value); }

    // MIDI Learn: the plugin-GUI knob the user moved last (-1 = none).
    int lastTouchedParamIndex() const noexcept
        { return instance != nullptr ? instance->lastTouchedParamIndex() : -1; }

protected:
    // MIDI binding: 0..1 fraction → the parameter's own min..max range.
    void applyParamBinding (uint32_t paramIndex, float frac) override
    {
        const auto* p = paramInfo ((int) paramIndex);
        if (p == nullptr) return;
        setParamValue (p->id, p->minValue + (double) frac * (p->maxValue - p->minValue));
    }
};
} // namespace duskstudio::clap

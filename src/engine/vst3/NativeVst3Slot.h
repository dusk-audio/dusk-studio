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

// VST3 insert slot — the shared NativeInsertSlot plus parameter forwarding
// (normalized values; mirrors NativeClapSlot's surface).
class NativeVst3Slot final : public hosting::NativeInsertSlot<Vst3SlotTraits>
{
public:
    // Parameters (message thread for read/enumerate; setParamValue is the control
    // entry — queued and applied on the next audio block). No-op / empty when unloaded.
    int paramCount() const noexcept { return instance != nullptr ? instance->paramCount() : 0; }
    const Vst3Instance::ParamInfo* paramInfo (int index) const noexcept
        { return instance != nullptr ? instance->paramInfo (index) : nullptr; }
    bool getParamValue (uint32_t id, double& out) const
        { return instance != nullptr && instance->getParamValue (id, out); }
    void setParamValue (uint32_t id, double value)
        { if (instance != nullptr) instance->setParamValue (id, value); }

    // MIDI Learn: the editor knob the user moved last (-1 = none).
    int lastTouchedParamIndex() const noexcept
        { return instance != nullptr ? instance->lastTouchedParamIndex() : -1; }

protected:
    // MIDI binding: VST3 parameters are normalized — the fraction maps directly.
    void applyParamBinding (uint32_t paramIndex, float frac) override
    {
        const auto* p = paramInfo ((int) paramIndex);
        if (p == nullptr) return;
        setParamValue (p->id, (double) frac);
    }
};
} // namespace duskstudio::vst3

#pragma once

#include "AuBundle.h"
#include "AuInstance.h"
#include "../hosting/NativeInsertSlot.h"

namespace duskstudio::au
{
struct AuSlotTraits
{
    using Bundle = AuBundle;
    using Instance = AuInstance;
    static constexpr const char* bundleNoun = "component";

    static bool pickPlugin (const AuBundle& bundle, const std::string& requestedId,
                            std::string& idOut, std::string& errorOut)
    {
        if (bundle.plugins().empty())
        {
            errorOut = "component has no Audio Unit";
            return false;
        }
        const auto stableId = bundle.plugins().front().id.toString();
        if (! requestedId.empty() && requestedId != stableId)
        {
            errorOut = "component identifier changed";
            return false;
        }
        idOut = stableId;
        return true;
    }
};

class NativeAuSlot final : public hosting::NativeInsertSlot<AuSlotTraits>
{
public:
    std::string displayName() const
    {
        return bundle != nullptr && ! bundle->plugins().empty()
            ? bundle->plugins().front().name
            : std::string {};
    }
    int paramCount() const noexcept { return instance != nullptr ? instance->paramCount() : 0; }
    const AuInstance::ParamInfo* paramInfo (int index) const noexcept
        { return instance != nullptr ? instance->paramInfo (index) : nullptr; }
    bool getParamValue (std::uint32_t id, double& out) const
        { return instance != nullptr && instance->getParamValue (id, out); }
    void setParamValue (std::uint32_t id, double value)
        { if (instance != nullptr) instance->setParamValue (id, value); }
    int lastTouchedParamIndex() const noexcept
        { return instance != nullptr ? instance->lastTouchedParamIndex() : -1; }
    bool refreshLatencyIfChanged() noexcept
        { return instance != nullptr && instance->refreshLatencyIfChanged(); }

protected:
    void applyParamBinding (std::uint32_t paramIndex, float fraction) override
    {
        const auto* parameter = paramInfo (static_cast<int> (paramIndex));
        if (parameter == nullptr || ! parameter->writable) return;
        setParamValue (parameter->id, parameter->minValue
            + static_cast<double> (fraction) * (parameter->maxValue - parameter->minValue));
    }
};
} // namespace duskstudio::au

#pragma once

#include "BuiltinBundle.h"
#include "BuiltinInstance.h"
#include "../hosting/NativeInsertSlot.h"

#include <string>

namespace duskstudio::builtin
{
struct BuiltinSlotTraits
{
    using Bundle   = BuiltinBundle;
    using Instance = BuiltinInstance;
    static constexpr const char* bundleNoun = "built-in unit";

    // One unit per bundle: the bundle IS the registry entry, so the only
    // acceptable id is its own.
    static bool pickPlugin (const BuiltinBundle& bundle, const std::string& requestedId,
                            std::string& idOut, std::string& errorOut)
    {
        const auto* unit = bundle.unit();
        if (unit == nullptr) { errorOut = "no unit in bundle"; return false; }
        if (! requestedId.empty() && requestedId != unit->id)
        {
            errorOut = "built-in unit '" + std::string (unit->id)
                     + "' does not provide '" + requestedId + "'";
            return false;
        }
        idOut = unit->id;
        return true;
    }
};

// Built-in insert slot - the shared NativeInsertSlot plus the unit's parameter
// surface (message thread; the audio thread reads it through relaxed atomics
// inside the unit).
class NativeBuiltinSlot final : public hosting::NativeInsertSlot<BuiltinSlotTraits>
{
public:
    // Message thread. The unit id is the slot's identity, so the bundle "path"
    // and the plugin id are the same string.
    bool loadUnit (const std::string& unitId, double sampleRate, int maxBlock,
                   std::string& errorOut)
    {
        return load (std::filesystem::u8path (unitId), sampleRate, maxBlock,
                     errorOut, unitId);
    }

    std::string displayName() const
    {
        if (instance != nullptr) return instance->displayName();
        const auto* unit = findUnit (loadedPluginId);
        return unit != nullptr ? unit->name : loadedPluginId;
    }

    int paramCount() const noexcept { return instance != nullptr ? instance->paramCount() : 0; }
    const ParamInfo* paramInfo (int index) const noexcept
        { return instance != nullptr ? instance->paramInfo (index) : nullptr; }
    float getParamValue (int index) const noexcept
        { return instance != nullptr ? instance->getParamValue (index) : 0.0f; }
    void setParamValue (int index, float value) noexcept
        { if (instance != nullptr) instance->setParamValue (index, value); }
};
} // namespace duskstudio::builtin

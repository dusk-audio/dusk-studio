#pragma once

#include "BuiltinBundle.h"
#include "BuiltinInstance.h"
#include "../hosting/NativeInsertSlot.h"

#include <atomic>
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
        lastTouched.store (-1, std::memory_order_relaxed);
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

    // MIDI Learn: the control the user moved last in this unit's editor, or -1
    // when none has been touched since it loaded. Written by the editor on the
    // message thread, read by the learn resolver on the same thread.
    int lastTouchedParamIndex() const noexcept
        { return lastTouched.load (std::memory_order_relaxed); }
    void noteParamTouched (int index) noexcept
        { lastTouched.store (index, std::memory_order_relaxed); }

protected:
    // MIDI binding: a 0..1 fraction maps onto the parameter's own range.
    void applyParamBinding (uint32_t paramIndex, float frac) override
    {
        const auto* p = paramInfo ((int) paramIndex);
        if (p == nullptr) return;
        setParamValue ((int) paramIndex,
                       p->minValue + frac * (p->maxValue - p->minValue));
    }

private:
    std::atomic<int> lastTouched { -1 };
};
} // namespace duskstudio::builtin

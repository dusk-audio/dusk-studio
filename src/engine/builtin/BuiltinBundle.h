#pragma once

#include "BuiltinRegistry.h"

#include <string>

namespace duskstudio::builtin
{
// The "bundle" of a built-in unit is its registry entry, so load() is a lookup
// and touches no disk. It exists so the built-in rung satisfies the same
// NativeInsertSlot traits contract as the CLAP / LV2 / VST3 rungs.
class BuiltinBundle
{
public:
    bool load (const std::string& id, std::string& errorOut)
    {
        info = findUnit (id);
        if (info == nullptr)
        {
            errorOut = "no built-in unit with id '" + id + "'";
            return false;
        }
        return true;
    }

    const UnitInfo* unit() const noexcept { return info; }

private:
    const UnitInfo* info = nullptr;
};
} // namespace duskstudio::builtin

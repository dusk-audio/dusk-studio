#include "BuiltinRegistry.h"

#include "UtilityUnit.h"

namespace duskstudio::builtin
{
const std::vector<UnitInfo>& registry()
{
    static const std::vector<UnitInfo> units
    {
        { "dusk.builtin.utility", "Utility", "Fx|Utility", false },
    };
    return units;
}

const UnitInfo* findUnit (const std::string& id)
{
    for (const auto& unit : registry())
        if (id == unit.id) return &unit;
    return nullptr;
}

std::unique_ptr<BuiltinUnit> createUnit (const std::string& id)
{
    if (id == "dusk.builtin.utility") return std::make_unique<UtilityUnit>();
    return nullptr;
}
} // namespace duskstudio::builtin

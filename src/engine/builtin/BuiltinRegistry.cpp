#include "BuiltinRegistry.h"

#include "UtilityUnit.h"
#if DUSKSTUDIO_HAS_DONOR_UNITS
 #include "DelayUnit.h"
 #include "ReverbUnit.h"
 #include "TapeUnit.h"
#endif

namespace duskstudio::builtin
{
const std::vector<UnitInfo>& registry()
{
    static const std::vector<UnitInfo> units
    {
        { "dusk.builtin.utility", "Utility", "Fx|Utility", false,
          [] () -> std::unique_ptr<BuiltinUnit> { return std::make_unique<UtilityUnit>(); } },
#if DUSKSTUDIO_HAS_DONOR_UNITS
        { "dusk.builtin.reverb", "Reverb", "Fx|Reverb", false,
          [] () -> std::unique_ptr<BuiltinUnit> { return std::make_unique<ReverbUnit>(); } },
        { "dusk.builtin.delay", "Tape Echo", "Fx|Delay", false,
          [] () -> std::unique_ptr<BuiltinUnit> { return std::make_unique<DelayUnit>(); } },
        { "dusk.builtin.tape", "Tape", "Fx|Distortion", false,
          [] () -> std::unique_ptr<BuiltinUnit> { return std::make_unique<TapeUnit>(); } },
#endif
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
    const auto* unit = findUnit (id);
    return unit != nullptr && unit->create != nullptr ? unit->create() : nullptr;
}
} // namespace duskstudio::builtin

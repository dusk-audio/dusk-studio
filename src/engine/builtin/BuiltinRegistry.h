#pragma once

#include "BuiltinUnit.h"
#include "DafPlugin.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::builtin
{
// A unit in the built-in suite. `id` is the stable session key and the picker
// row's location; it never changes once a session can hold it. The factory
// lives here so the registry is the only place a new unit is declared: `create`
// for a knob unit, `createPlugin` for a unit that is one of Dusk's own DAF
// plug-ins run in process.
struct UnitInfo
{
    const char* id;
    const char* name;
    const char* category;
    bool isInstrument;
    std::unique_ptr<BuiltinUnit> (*create)();
    std::unique_ptr<DafPlugin> (*createPlugin)() = nullptr;
    // For a plug-in unit that replaced a knob unit under this id, in the knob
    // unit's parameter order, which is the index a binding saved against it names.
    const std::vector<LegacyParam>* legacyParams = nullptr;
};

// The suite, in picker order. Compiled in, so it is identical on every
// platform and needs no scan.
const std::vector<UnitInfo>& registry();

// nullptr when no unit carries that id (a session written by a newer build).
const UnitInfo* findUnit (const std::string& id);

// nullptr for an unknown id or a DAF plug-in unit. Message thread - the unit
// allocates.
std::unique_ptr<BuiltinUnit> createUnit (const std::string& id);

// When `state` is a blob the knob unit wrote before a plug-in took over its id:
// for each of the knob unit's parameters, in its order, the index of the plug-in
// parameter that replaced it, or -1. Empty for any other state. Message thread -
// it builds the plug-in to read its parameter order.
std::vector<int> knobUnitParamIndices (const std::string& unitId,
                                       const std::vector<std::uint8_t>& state);

// What picker rows and the session's "which plugin is this" surfaces show.
constexpr const char* kManufacturer = "Dusk Audio";
constexpr const char* kFormatName   = "Builtin";
} // namespace duskstudio::builtin

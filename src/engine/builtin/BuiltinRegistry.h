#pragma once

#include "BuiltinUnit.h"

#include <memory>
#include <string>
#include <vector>

namespace duskstudio::builtin
{
// A unit in the built-in suite. `id` is the stable session key and the picker
// row's location; it never changes once a session can hold it. The factory
// lives here so the registry is the only place a new unit is declared.
struct UnitInfo
{
    const char* id;
    const char* name;
    const char* category;
    bool isInstrument;
    std::unique_ptr<BuiltinUnit> (*create)();
};

// The suite, in picker order. Compiled in, so it is identical on every
// platform and needs no scan.
const std::vector<UnitInfo>& registry();

// nullptr when no unit carries that id (a session written by a newer build).
const UnitInfo* findUnit (const std::string& id);

// nullptr for an unknown id. Message thread - the unit allocates.
std::unique_ptr<BuiltinUnit> createUnit (const std::string& id);

// What picker rows and the session's "which plugin is this" surfaces show.
constexpr const char* kManufacturer = "Dusk Audio";
constexpr const char* kFormatName   = "Builtin";
} // namespace duskstudio::builtin

#pragma once

#include "BuiltinRegistry.h"
#include "../PluginDescriptor.h"

#include <vector>

namespace duskstudio::builtin
{
// Picker rows for the built-in suite. There is nothing to scan: the registry is
// compiled in, so the rows are the same on every machine. `location` carries the
// unit id, which is what a slot loads and what the session persists.
inline std::vector<PluginDescriptor> descriptorRows (bool instruments)
{
    std::vector<PluginDescriptor> rows;
    for (const auto& unit : registry())
    {
        if (unit.isInstrument != instruments) continue;
        PluginDescriptor descriptor;
        descriptor.name = unit.name;
        descriptor.manufacturer = kManufacturer;
        descriptor.category = unit.category;
        descriptor.formatName = kFormatName;
        descriptor.backend = PluginBackend::Native;
        descriptor.location = unit.id;
        descriptor.isInstrument = unit.isInstrument;
        rows.push_back (std::move (descriptor));
    }
    return rows;
}
} // namespace duskstudio::builtin

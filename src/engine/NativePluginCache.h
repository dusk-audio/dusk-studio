#pragma once

#include "PluginDescriptor.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace duskstudio::nativecache
{
using LocationExists = std::function<bool (std::string_view)>;

std::string serialize (const std::vector<PluginDescriptor>& descriptors);

// Parses a complete cache document into `out`. Invalid cache schemas fail
// without changing `out`; invalid descriptor rows and stale locations are
// skipped independently so one bad plugin cannot discard the rest of a scan.
bool parse (std::string_view source, const LocationExists& locationExists,
            std::vector<PluginDescriptor>& out);
} // namespace duskstudio::nativecache

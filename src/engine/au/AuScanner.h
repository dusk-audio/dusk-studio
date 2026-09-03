#pragma once

#include "../PluginDescriptor.h"

#include <atomic>
#include <vector>

namespace duskstudio::au
{
// Registry-only discovery: AudioComponentFindNext and metadata reads do not
// instantiate third-party code, matching the native LV2 manifest-scan tier.
class AuScanner
{
public:
    static std::vector<PluginDescriptor> scan (const std::atomic<bool>* abort = nullptr);
};
} // namespace duskstudio::au

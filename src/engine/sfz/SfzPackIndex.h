#pragma once

#include "SfzProvider.h"

#include <filesystem>
#include <vector>

namespace duskstudio::sfz
{
// Receipt entries remain the complete identity list; this is a browser projection.
std::vector<ProviderInstrument> indexPackPrograms (
    const std::filesystem::path& directory, const std::vector<ProviderInstrument>& instruments);
} // namespace duskstudio::sfz

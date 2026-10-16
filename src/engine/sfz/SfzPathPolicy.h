#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace duskstudio::sfz
{
struct PathLimits
{
    std::size_t maxBytes = 512;
    std::size_t maxDepth = 32;
    std::size_t maxSegmentBytes = 255;
};

bool validateRelativePath (std::string_view path, std::string& error,
                           const PathLimits& limits = {});
bool isSha256 (std::string_view value) noexcept;

// A comparison key only: the original UTF-8 spelling is retained on disk.
std::optional<std::string> portablePathKey (std::string_view path);
} // namespace duskstudio::sfz

#pragma once

#include "foundation/Fs.h"

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace duskstudio::test
{
class TempDirectory
{
public:
    explicit TempDirectory (std::string_view prefix)
        : value (dusk::fs::createUniqueTempDirectory (prefix))
    {
        if (value.empty()) throw std::runtime_error ("could not allocate test directory");
    }

    ~TempDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all (value, ignored);
    }

    const std::filesystem::path& path() const noexcept { return value; }

private:
    std::filesystem::path value;
};
} // namespace duskstudio::test

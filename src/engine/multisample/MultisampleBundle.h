#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>

namespace duskstudio
{
// The "bundle" for the native multisample rung is the soundfont file itself -
// there is no shared library to open, so load() only validates the path. It
// exists so the multisample slot satisfies the same NativeInsertSlot traits
// contract as the CLAP / LV2 / VST3 rungs.
class MultisampleBundle
{
public:
    static bool isSoundfontExtension (const std::filesystem::path& p)
    {
        auto ext = p.extension().u8string();
        std::transform (ext.begin(), ext.end(), ext.begin(),
                        [] (unsigned char c) { return (char) std::tolower (c); });
        return ext == ".sfz" || ext == ".sf2";
    }

    bool load (const std::string& path, std::string& errorOut)
    {
        auto p = std::filesystem::u8path (path);
        if (! isSoundfontExtension (p))
        {
            errorOut = "not a soundfont (.sfz / .sf2): " + path;
            return false;
        }
        std::error_code ec;
        if (! std::filesystem::is_regular_file (p, ec))
        {
            errorOut = "file does not exist: " + path;
            return false;
        }
        file = std::move (p);
        return true;
    }

    const std::filesystem::path& getFile() const noexcept { return file; }

private:
    std::filesystem::path file;
};
} // namespace duskstudio

#pragma once

#include "../foundation/Fs.h"

#include <filesystem>
#include <string>
#include <system_error>

// A desktop drop names files by whatever the platform's drag hands over, which
// on Linux has included web links that end in .wav. Only an existing local file
// the tape strip can import is worth a picker or an import error.
namespace duskstudio::droppedfiles
{
inline bool hasImportableExtension (const std::filesystem::path& path)
{
    for (const char* ext : { "wav", "aiff", "aif", "flac", "mid", "midi" })
        if (dusk::fs::hasExtension (path, ext))
            return true;
    return false;
}

inline bool isImportableFile (const std::string& utf8Path)
{
    if (utf8Path.empty()) return false;
    const auto path = std::filesystem::u8path (utf8Path);
    std::error_code ec;
    return path.is_absolute() && hasImportableExtension (path)
        && std::filesystem::is_regular_file (path, ec);
}
} // namespace duskstudio::droppedfiles

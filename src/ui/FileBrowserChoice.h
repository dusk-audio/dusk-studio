#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace duskstudio::filebrowser
{
// Save mode adds a name field at the bottom + warns about overwriting
// existing files.
enum class Mode { Open, Save };

// Whether a path the browser reports may be handed back when the user
// presses Open or Save. A file Open takes only a file that is there: the
// browser reports its own folder, or a start path that never existed, when
// nothing was picked. Save takes a new name, and directory pickers keep the
// browser's answer.
inline bool isAcceptableChoice (const std::string& utf8Path, Mode mode, bool selectDirectories)
{
    if (utf8Path.empty()) return false;
    if (mode == Mode::Save || selectDirectories) return true;
    std::error_code ec;
    return std::filesystem::is_regular_file (std::filesystem::u8path (utf8Path), ec);
}
} // namespace duskstudio::filebrowser

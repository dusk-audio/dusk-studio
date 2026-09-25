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
// nothing was picked. A file Save takes any name but a folder's, which is
// what the browser reports for an empty name box. Folder pickers and saves
// whose answer is a folder keep the browser's answer.
inline bool isAcceptableChoice (const std::string& utf8Path, Mode mode,
                                bool selectDirectories, bool saveAnswerIsFolder)
{
    if (utf8Path.empty()) return false;
    if (selectDirectories || (mode == Mode::Save && saveAnswerIsFolder)) return true;
    std::error_code ec;
    const auto path = std::filesystem::u8path (utf8Path);
    if (mode == Mode::Save) return ! std::filesystem::is_directory (path, ec);
    return std::filesystem::is_regular_file (path, ec);
}
} // namespace duskstudio::filebrowser

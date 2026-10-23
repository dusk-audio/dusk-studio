#include "FileBrowserChoice.h"
#include "../foundation/Fs.h"

#include <filesystem>
#include <system_error>

namespace duskstudio::filebrowser
{
bool isAcceptableChoice (const std::string& utf8Path, Mode mode,
                         bool selectDirectories, bool saveAnswerIsFolder)
{
    if (utf8Path.empty()) return false;
    if (selectDirectories || (mode == Mode::Save && saveAnswerIsFolder)) return true;
    std::error_code ec;
    const auto path = dusk::fs::nativeIOPath (std::filesystem::u8path (utf8Path));
    if (mode == Mode::Save) return ! std::filesystem::is_directory (path, ec);
    return std::filesystem::is_regular_file (path, ec);
}
} // namespace duskstudio::filebrowser

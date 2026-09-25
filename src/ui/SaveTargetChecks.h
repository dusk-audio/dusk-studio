#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace duskstudio::savecheck
{
inline constexpr const char* kReplaceFileTitle   = "Replace file?";
inline constexpr const char* kReplaceFileButton  = "Replace";
inline constexpr const char* kOtherSessionTitle  = "Folder holds another session";

inline std::string replaceFileMessage (const std::string& fileName, const std::string& note = {})
{
    return "This file already exists and will be replaced:\n\n" + fileName + "\n\n"
         + (note.empty() ? std::string() : note + "\n\n") + "Continue?";
}

inline std::string otherSessionMessage (const std::string& folder)
{
    return "This folder already holds another session:\n\n    " + folder + "\n\n"
           "Saving here would replace it, so nothing was saved and nothing was changed. "
           "Choose a new name, or a folder without a session.";
}

// A folder reached through a link, or spelled differently, is still the
// session's own folder, so the comparison is by identity, not by text.
inline bool holdsAnotherSession (const std::filesystem::path& targetDir,
                                 const std::filesystem::path& currentDir)
{
    std::error_code ec;
    if (! std::filesystem::is_regular_file (targetDir / "session.json", ec)) return false;
    return ! std::filesystem::equivalent (targetDir, currentDir, ec);
}
} // namespace duskstudio::savecheck

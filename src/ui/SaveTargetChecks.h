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
inline bool isSameFolder (const std::filesystem::path& a, const std::filesystem::path& b)
{
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    std::error_code ec;
    return std::filesystem::equivalent (a, b, ec);
}

inline bool holdsAnotherSession (const std::filesystem::path& targetDir,
                                 const std::filesystem::path& currentDir)
{
    std::error_code ec;
    if (! std::filesystem::is_regular_file (targetDir / "session.json", ec)) return false;
    return ! isSameFolder (targetDir, currentDir);
}

// Anything short of a clear "no session.json there" counts as holding one.
inline bool mayHoldSession (const std::filesystem::path& dir)
{
    std::error_code ec;
    return std::filesystem::symlink_status (dir / "session.json", ec).type()
           != std::filesystem::file_type::not_found;
}

// The folder a never-saved session starts in: parent/Untitled, or the first
// "Untitled N" beside it that holds no session. Empty when none qualifies.
inline std::filesystem::path unsavedSessionFolder (const std::filesystem::path& parent)
{
    for (int n = 1; n < 100; ++n)
    {
        const auto dir = parent / (n == 1 ? std::string ("Untitled") : "Untitled " + std::to_string (n));
        if (! mayHoldSession (dir)) return dir;
    }
    return {};
}

// Where a session keeps its autosave and notepad. A never-saved session starts
// in a folder that holds no session, but a session.json can still appear there
// later from outside; its files then go to privateDir instead.
inline std::filesystem::path sidecarFolder (const std::filesystem::path& sessionDir,
                                            bool savedOrOpened,
                                            const std::filesystem::path& privateDir)
{
    if (savedOrOpened || sessionDir.empty() || ! mayHoldSession (sessionDir)) return sessionDir;
    return privateDir;
}
} // namespace duskstudio::savecheck

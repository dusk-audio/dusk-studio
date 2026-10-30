#pragma once

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <cstdio>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace duskstudio::savecheck
{
inline constexpr const char* kReplaceFileTitle   = "Replace file?";
inline constexpr const char* kReplaceFileButton  = "Replace";
inline constexpr const char* kOtherSessionTitle  = "Folder holds another session";
inline constexpr const char* kLoadedMixTitle     = "File is the loaded mix";

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

inline std::string loadedMixMessage (const std::string& fileName)
{
    return "This file is the mix the master is rendered from:\n\n    " + fileName + "\n\n"
           "Exporting over it would destroy the mix, so nothing was exported and nothing was changed. "
           "Choose another name for the master.";
}

// A file or folder reached through a link, spelled differently, or named in
// another case on a filesystem that ignores case is still the same one, so
// the comparison is by identity, not by text.
inline bool isSameLocation (const std::filesystem::path& a, const std::filesystem::path& b)
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
    return ! isSameLocation (targetDir, currentDir);
}

// Anything short of a clear "no session.json there" counts as holding one.
inline bool mayHoldSession (const std::filesystem::path& dir)
{
    std::error_code ec;
    return std::filesystem::symlink_status (dir / "session.json", ec).type()
           != std::filesystem::file_type::not_found;
}

// Makes file only when nothing is at that path, without following a link
// there. 0 when made, else the errno of the failure.
inline int createNewFile (const std::filesystem::path& file)
{
#if defined(_WIN32)
    std::FILE* stream = nullptr;
    if (const auto err = _wfopen_s (&stream, file.c_str(), L"wx")) return err;
    return std::fclose (stream) == 0 ? 0 : EIO;
#else
    const int fd = ::open (file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    return ::close (fd) == 0 ? 0 : errno;
#endif
}

// Whether a file can be made and removed in dir, found by doing it. Permission
// bits cannot tell: Windows marks folders such as Music read-only without
// stopping writes, and a read-only mount or an ACL stops them while the bits
// allow it. A Windows ACL can also allow folders and refuse files, so the
// probe is a file.
inline bool canCreateFileIn (const std::filesystem::path& dir)
{
#if defined(_WIN32)
    const auto process = std::to_string (_getpid());
#else
    const auto process = std::to_string ((long) ::getpid());
#endif
    const auto tick = std::to_string (std::chrono::steady_clock::now().time_since_epoch().count());

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const auto probe = dir / (".dusk-studio-write-check-" + process + "-" + tick + "-"
                                  + std::to_string (attempt));
        const int err = createNewFile (probe);
        if (err == EEXIST) continue;
        if (err != 0) return false;
        std::error_code ec;
        return std::filesystem::remove (probe, ec) && ! ec;
    }
    return false;
}

inline bool makeReadyForSession (const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::create_directories (dir / "audio", ec);
    return ! ec && canCreateFileIn (dir) && canCreateFileIn (dir / "audio");
}

// The folder a never-saved session starts in, made with its audio folder:
// parent/Untitled, or the first "Untitled N" beside it without saved state,
// recovery or notes. One whose contents cannot be checked counts as holding
// them. One is passed over when it cannot take an audio folder, or when it or
// its audio folder cannot be written in. Creates parent if needed; empty when
// it cannot, or when parent cannot be searched or written in.
inline std::filesystem::path unsavedSessionFolder (const std::filesystem::path& parent)
{
    if (parent.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories (parent, ec);
    if (! std::filesystem::is_directory (parent, ec)) return {};

    for (unsigned int n = 1; n != 0; ++n)
    {
        const auto dir = parent / (n == 1 ? std::string ("Untitled") : "Untitled " + std::to_string (n));
        const auto status = std::filesystem::symlink_status (dir, ec);
        if (status.type() == std::filesystem::file_type::not_found)
            return makeReadyForSession (dir) ? dir : std::filesystem::path();
        if (ec) return {};
        if (! std::filesystem::is_directory (status)) continue;

        bool occupied = false;
        for (const auto* name : { "session.json", "session.json.autosave", "notepad.md" })
        {
            const auto fileStatus = std::filesystem::symlink_status (dir / name, ec);
            if (fileStatus.type() == std::filesystem::file_type::not_found) continue;
            occupied = true;
            break;
        }
        if (occupied) continue;
        if (makeReadyForSession (dir)) return dir;
    }
    return {};
}

// The launch session's folder: a free Untitled folder in parent, else in
// privateDir, else in the private temporary folder makeTempDir creates, asked
// for only when both fail. Empty when none of them can hold one.
template <typename MakeTempDir>
std::filesystem::path launchSessionFolder (const std::filesystem::path& parent,
                                           const std::filesystem::path& privateDir,
                                           MakeTempDir&& makeTempDir)
{
    auto dir = unsavedSessionFolder (parent);
    if (dir.empty()) dir = unsavedSessionFolder (privateDir);
    if (dir.empty()) dir = unsavedSessionFolder (makeTempDir());
    return dir;
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

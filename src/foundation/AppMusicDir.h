#pragma once

#include "AppConfigDir.h"
#include "Fs.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

// The folder Dusk Studio treats as the user's Music folder: its "Dusk Studio"
// child is where a session starts and where Save As and New Session first look,
// and the import and bounce browsers fall back to it.
//
// DUSKSTUDIO_MUSIC_DIR names it outright. Without it, a scenario or self-test
// run gets a fresh directory of its own, removed at exit, so the launch
// session's autosaves, takes and bounces never land among the user's sessions.
namespace dusk::fs
{
inline constexpr const char* kMusicDirEnv = "DUSKSTUDIO_MUSIC_DIR";

// userMusicDir is only asked on a normal launch, so a harness run never so much
// as looks at the user's folder.
template <typename UserMusicDir, typename MakeHarnessDir>
std::filesystem::path resolveAppMusicDir (const std::filesystem::path& overrideDir,
                                          bool harnessRun,
                                          UserMusicDir&& userMusicDir,
                                          MakeHarnessDir&& makeHarnessDir)
{
    if (! overrideDir.empty())
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute (overrideDir, error);
        return error ? overrideDir : absolute;
    }
    if (harnessRun) return makeHarnessDir();
    return userMusicDir();
}

namespace detail
{
inline std::filesystem::path musicDirOverride()
{
   #if defined(_WIN32)
    if (const wchar_t* dir = ::_wgetenv (L"DUSKSTUDIO_MUSIC_DIR"))
        return std::filesystem::path (dir);
   #else
    if (const char* dir = std::getenv (kMusicDirEnv))
        return std::filesystem::u8path (dir);
   #endif
    return {};
}

// A home without a Music folder keeps its sessions in the home folder itself.
inline std::filesystem::path userMusicOrHomeDir()
{
    const auto music = userMusicDir();
    std::error_code error;
    if (! music.empty() && std::filesystem::is_directory (music, error)) return music;
    return userHomeDir();
}

inline std::filesystem::path harnessMusicDir()
{
    struct Owned
    {
        std::filesystem::path dir = createUniqueTempDirectory ("dusk-studio-music-");
        ~Owned()
        {
            std::error_code error;
            if (! dir.empty()) std::filesystem::remove_all (dir, error);
        }
    };
    static const Owned owned;
    return owned.dir;
}
} // namespace detail

// Empty only when there is no home folder at all.
inline std::filesystem::path appMusicDir()
{
    return resolveAppMusicDir (detail::musicDirOverride(),
                               isHarnessRun (std::getenv ("DUSKSTUDIO_RUN_SCENARIOS"),
                                             std::getenv ("DUSKSTUDIO_RUN_SELFTEST")),
                               detail::userMusicOrHomeDir,
                               detail::harnessMusicDir);
}
} // namespace dusk::fs

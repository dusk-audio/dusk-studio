#pragma once

#include "Fs.h"
#include "Text.h"

#include <cstdlib>
#include <filesystem>
#include <string>

// The one directory Dusk Studio keeps its per-user files in: window state,
// app config, recent sessions, the audio device choice, plugin caches, logs.
//
// DUSKSTUDIO_CONFIG_DIR names it outright. Without it, a scenario or self-test
// run gets a fresh directory of its own, removed at exit, so a harness run
// neither depends on nor rewrites the user's saved settings.
namespace dusk::fs
{
inline constexpr const char* kConfigDirEnv = "DUSKSTUDIO_CONFIG_DIR";

// Mirrors the app's own test for the two harness switches, so the config is
// isolated exactly when the app takes the harness path.
inline bool isHarnessRun (const char* runScenarios, const char* runSelfTest)
{
    if (runScenarios != nullptr && *runScenarios != '\0') return true;
    if (runSelfTest == nullptr) return false;
    const std::string flag (runSelfTest);
    const auto lower = dusk::text::toLowerCase (dusk::text::trim (flag));
    return dusk::text::getIntValue (flag) != 0 || lower == "true" || lower == "yes";
}

template <typename MakeHarnessDir>
std::filesystem::path resolveAppConfigDir (const std::filesystem::path& overrideDir,
                                           bool harnessRun,
                                           const std::filesystem::path& userConfigRoot,
                                           MakeHarnessDir&& makeHarnessDir)
{
    if (! overrideDir.empty())
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute (overrideDir, error);
        return error ? overrideDir : absolute;
    }
    if (harnessRun) return makeHarnessDir();
    return userConfigRoot.empty() ? std::filesystem::path {} : userConfigRoot / "Dusk Studio";
}

namespace detail
{
inline std::filesystem::path configDirOverride()
{
   #if defined(_WIN32)
    if (const wchar_t* dir = ::_wgetenv (L"DUSKSTUDIO_CONFIG_DIR"))
        return std::filesystem::path (dir);
   #else
    if (const char* dir = std::getenv (kConfigDirEnv))
        return std::filesystem::u8path (dir);
   #endif
    return {};
}

inline std::filesystem::path harnessConfigDir()
{
    struct Owned
    {
        std::filesystem::path dir = createUniqueTempDirectory ("dusk-studio-config-");
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

// Empty when there is nowhere to keep the files; callers then skip persisting.
inline std::filesystem::path appConfigDir()
{
    return resolveAppConfigDir (detail::configDirOverride(),
                                isHarnessRun (std::getenv ("DUSKSTUDIO_RUN_SCENARIOS"),
                                              std::getenv ("DUSKSTUDIO_RUN_SELFTEST")),
                                userConfigDir(),
                                detail::harnessConfigDir);
}
} // namespace dusk::fs

#include <catch2/catch_test_macros.hpp>

#include "foundation/AppConfigDir.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace stdfs = std::filesystem;

namespace
{
const stdfs::path kUserRoot = stdfs::temp_directory_path() / "dusk-app-config-user-root";
const stdfs::path kHarnessDir = stdfs::temp_directory_path() / "dusk-app-config-harness";

stdfs::path harnessDir() { return kHarnessDir; }

class ScopedEnv
{
public:
    ScopedEnv (const char* nameIn, const std::string& value) : name (nameIn)
    {
        if (const char* existing = std::getenv (name))
        {
            hadPrevious = true;
            previous = existing;
        }
        set (value.c_str());
    }

    ~ScopedEnv() { set (hadPrevious ? previous.c_str() : nullptr); }

    ScopedEnv (const ScopedEnv&) = delete;
    ScopedEnv& operator= (const ScopedEnv&) = delete;

private:
    void set (const char* value)
    {
       #if defined(_WIN32)
        ::_putenv_s (name, value != nullptr ? value : "");
       #else
        if (value != nullptr) ::setenv (name, value, 1);
        else                  ::unsetenv (name);
       #endif
    }

    const char* name;
    std::string previous;
    bool hadPrevious = false;
};
} // namespace

TEST_CASE ("A normal launch keeps its files in the user's Dusk Studio directory", "[appconfigdir]")
{
    bool madeHarnessDir = false;
    const auto dir = dusk::fs::resolveAppConfigDir ({}, false, kUserRoot,
                                                    [&] { madeHarnessDir = true; return kHarnessDir; });
    REQUIRE (dir == kUserRoot / "Dusk Studio");
    REQUIRE_FALSE (madeHarnessDir);

    REQUIRE (dusk::fs::resolveAppConfigDir ({}, false, {}, harnessDir).empty());
}

TEST_CASE ("A harness run never resolves to the user's directory", "[appconfigdir]")
{
    REQUIRE (dusk::fs::resolveAppConfigDir ({}, true, kUserRoot, harnessDir) == kHarnessDir);
}

TEST_CASE ("An explicit config directory wins over both", "[appconfigdir]")
{
    const auto chosen = stdfs::temp_directory_path() / "dusk-app-config-chosen";
    REQUIRE (dusk::fs::resolveAppConfigDir (chosen, true, kUserRoot, harnessDir) == chosen);
    REQUIRE (dusk::fs::resolveAppConfigDir (chosen, false, kUserRoot, harnessDir) == chosen);

    const auto relative = dusk::fs::resolveAppConfigDir ("relative-config", false, kUserRoot, harnessDir);
    REQUIRE (relative.is_absolute());
    REQUIRE (relative.filename() == "relative-config");
}

TEST_CASE ("The harness switches read the way the app reads them", "[appconfigdir]")
{
    using dusk::fs::isHarnessRun;
    REQUIRE (isHarnessRun ("all", nullptr));
    REQUIRE (isHarnessRun ("gui:gui.piano_edit_keys", nullptr));
    REQUIRE_FALSE (isHarnessRun ("", nullptr));
    REQUIRE_FALSE (isHarnessRun (nullptr, nullptr));

    REQUIRE (isHarnessRun (nullptr, "1"));
    REQUIRE (isHarnessRun (nullptr, "true"));
    REQUIRE (isHarnessRun (nullptr, "YES"));
    REQUIRE_FALSE (isHarnessRun (nullptr, "0"));
    REQUIRE_FALSE (isHarnessRun (nullptr, ""));
    REQUIRE_FALSE (isHarnessRun (nullptr, "no"));
    REQUIRE_FALSE (isHarnessRun ("", ""));
}

TEST_CASE ("A scenario run gets one private config directory for the whole process", "[appconfigdir]")
{
    const ScopedEnv noOverride (dusk::fs::kConfigDirEnv, "");
    const ScopedEnv scenarios ("DUSKSTUDIO_RUN_SCENARIOS", "gui");

    const auto dir = dusk::fs::appConfigDir();
    REQUIRE_FALSE (dir.empty());
    REQUIRE (dir != dusk::fs::userConfigDir() / "Dusk Studio");
    REQUIRE (stdfs::is_directory (dir));
    REQUIRE (dusk::fs::appConfigDir() == dir);
}

TEST_CASE ("DUSKSTUDIO_CONFIG_DIR names the config directory", "[appconfigdir]")
{
    const auto chosen = stdfs::temp_directory_path() / "dusk-app-config-env";
    const ScopedEnv override (dusk::fs::kConfigDirEnv, chosen.u8string());
    const ScopedEnv scenarios ("DUSKSTUDIO_RUN_SCENARIOS", "all");
    REQUIRE (dusk::fs::appConfigDir() == chosen);
}

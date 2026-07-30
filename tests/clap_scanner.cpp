// CLAP discovery: ClapScanner enumerates *.clap files + reads their descriptors.
// The path-shape cases run everywhere; the live load+enumerate case is gated on
// DUSKSTUDIO_TEST_CLAP=/path/to/Plugin.clap so CI without a CLAP plugin stays
// green. See docs/native-clap-host-plan.md.

#include <catch2/catch_test_macros.hpp>

#include "engine/clap/ClapScanner.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>

using duskstudio::clap::ClapScanner;

namespace
{
namespace stdfs = std::filesystem;

class TempDirectory
{
public:
    explicit TempDirectory (const char* prefix)
    {
        // pid + tick: ctest runs the test cases as parallel processes, whose
        // steady_clock reads can land on the same tick.
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        value = stdfs::temp_directory_path()
                / (std::string (prefix) + std::to_string (getpid()) + "_" + std::to_string (unique));
        std::error_code ec;
        if (! stdfs::create_directories (value, ec) || ec)
            throw std::runtime_error ("could not create test directory: " + ec.message());
    }

    ~TempDirectory()
    {
        std::error_code ec;
        stdfs::remove_all (value, ec);
    }

    const stdfs::path& path() const noexcept { return value; }

private:
    stdfs::path value;
};

class ScopedEnvironment
{
public:
    ScopedEnvironment (const char* variable, const std::string& value)
        : name (variable)
    {
        if (const char* current = std::getenv (variable))
            previous = current;
        if (setenv (name.c_str(), value.c_str(), 1) != 0)
            throw std::runtime_error ("could not set test environment variable");
    }

    ~ScopedEnvironment()
    {
        if (previous.has_value())
            setenv (name.c_str(), previous->c_str(), 1);
        else
            unsetenv (name.c_str());
    }

private:
    std::string name;
    std::optional<std::string> previous;
};

bool writeText (const stdfs::path& path, const char* text)
{
    std::ofstream stream (path);
    stream << text;
    return stream.good();
}
} // namespace

TEST_CASE ("ClapScanner default search paths are existing directories", "[clap][scan]")
{
    for (const auto& d : ClapScanner::defaultSearchPaths())
        REQUIRE (std::filesystem::is_directory (d));
}

TEST_CASE ("ClapScanner keeps CLAP_PATH ahead of platform defaults", "[clap][scan]")
{
    TempDirectory temp ("dusk_clap_scan_defaults_");
    const auto overridePath = temp.path() / "override";
    const auto home = temp.path() / "home";
#if defined(__APPLE__)
    const auto userDefault = home / "Library/Audio/Plug-Ins/CLAP";
#else
    const auto userDefault = home / ".clap";
#endif

    std::error_code ec;
    REQUIRE (stdfs::create_directories (overridePath, ec));
    REQUIRE_FALSE (ec);
    REQUIRE (stdfs::create_directories (userDefault, ec));
    REQUIRE_FALSE (ec);

    ScopedEnvironment scopedHome ("HOME", home.u8string());
    ScopedEnvironment scopedClapPath ("CLAP_PATH", overridePath.u8string());
    const auto paths = ClapScanner::defaultSearchPaths();
    REQUIRE (paths.size() >= 2);
    REQUIRE (paths[0] == overridePath);
    REQUIRE (paths[1] == userDefault);
}

TEST_CASE ("ClapScanner finds nothing in an empty directory", "[clap][scan]")
{
    TempDirectory temp ("dusk_clap_scan_empty_");
    REQUIRE (ClapScanner::findClapFiles ({ temp.path() }).empty());
    REQUIRE (ClapScanner::scan ({ temp.path() }).empty());
}

TEST_CASE ("ClapScanner collects this platform's CLAP bundle shape once", "[clap][scan]")
{
    TempDirectory temp ("dusk_clap_scan_shape_");
    const auto vendor = temp.path() / "Vendor";
    std::error_code ec;
    REQUIRE (stdfs::create_directory (vendor, ec));
    REQUIRE_FALSE (ec);

#if defined(__APPLE__)
    const auto bundle = vendor / "Outer.clap";
    REQUIRE (stdfs::create_directory (bundle, ec));
    REQUIRE_FALSE (ec);
    REQUIRE (stdfs::create_directory (bundle / "Nested.clap", ec));
    REQUIRE_FALSE (ec);
    REQUIRE (writeText (temp.path() / "NotABundle.clap", "regular file"));
#else
    const auto bundle = vendor / "Plugin.clap";
    REQUIRE (writeText (bundle, "not a loadable plugin"));
    REQUIRE (stdfs::create_directory (temp.path() / "NotAPlugin.clap", ec));
    REQUIRE_FALSE (ec);
#endif

    const auto found = ClapScanner::findClapFiles ({ temp.path(), temp.path() });
    REQUIRE (found.size() == 1);
    REQUIRE (found.front().is_absolute());
    REQUIRE (found.front() == bundle);
}

TEST_CASE ("ClapScanner discovers and describes a real CLAP bundle", "[clap][scan]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_CLAP not set — skipping live CLAP-scan test");
        return;
    }

    std::error_code ec;
    // Normalised: the scanner's bundle paths carry no trailing separator or "."
    // component, so an env value like ".../CLAP/Plugin.clap/" must not miss.
    // lexically_normal() resolves the dot elements but KEEPS a trailing
    // separator, which then shows up as an empty final filename.
    auto bundle = stdfs::absolute (stdfs::u8path (path), ec).lexically_normal();
    if (! bundle.has_filename()) bundle = bundle.parent_path();
    REQUIRE_FALSE (ec);
    REQUIRE (stdfs::exists (bundle));

    const auto found = ClapScanner::scan ({ bundle.parent_path() });
    REQUIRE_FALSE (found.empty());

    bool sawBundle = false;
    for (const auto& s : found)
    {
        if (s.bundlePath == bundle.u8string())
        {
            sawBundle = true;
            REQUIRE_FALSE (s.desc.id.empty());     // a usable plugin id to instantiate
            REQUIRE_FALSE (s.desc.name.empty());   // a human-readable name for the picker
        }
    }
    REQUIRE (sawBundle);
}

// VST3 discovery: Vst3Scanner enumerates *.vst3 bundles + reads their factory
// descriptors. The path-shape cases run everywhere; the live load+enumerate
// case is gated on DUSKSTUDIO_TEST_VST3=/path/to.vst3 so CI without a VST3
// plugin stays green.

#include <catch2/catch_test_macros.hpp>

#include "engine/vst3/Vst3Scanner.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>

using duskstudio::vst3::Vst3Scanner;

namespace
{
namespace stdfs = std::filesystem;

class TempDirectory
{
public:
    explicit TempDirectory (const char* prefix)
    {
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

TEST_CASE ("Vst3Scanner default search paths are existing directories", "[vst3][scan]")
{
    for (const auto& d : Vst3Scanner::defaultSearchPaths())
        REQUIRE (stdfs::is_directory (d));
}

TEST_CASE ("Vst3Scanner keeps VST3_PATH ahead of platform defaults", "[vst3][scan]")
{
    TempDirectory temp ("dusk_vst3_scan_defaults_");
    const auto overridePath = temp.path() / "override";
    const auto home = temp.path() / "home";
#if defined(__APPLE__)
    const auto userDefault = home / "Library/Audio/Plug-Ins/VST3";
#else
    const auto userDefault = home / ".vst3";
#endif

    std::error_code ec;
    REQUIRE (stdfs::create_directories (overridePath, ec));
    REQUIRE_FALSE (ec);
    REQUIRE (stdfs::create_directories (userDefault, ec));
    REQUIRE_FALSE (ec);

    ScopedEnvironment scopedHome ("HOME", home.u8string());
    ScopedEnvironment scopedVst3Path ("VST3_PATH", overridePath.u8string());
    const auto paths = Vst3Scanner::defaultSearchPaths();
    REQUIRE (paths.size() >= 2);
    REQUIRE (paths[0] == overridePath);
    REQUIRE (paths[1] == userDefault);
}

TEST_CASE ("Vst3Scanner finds nothing in an empty directory", "[vst3][scan]")
{
    TempDirectory temp ("dusk_vst3_scan_empty_");
    REQUIRE (Vst3Scanner::findVst3Bundles ({ temp.path() }).empty());
    REQUIRE (Vst3Scanner::scan ({ temp.path() }).empty());
}

TEST_CASE ("Vst3Scanner matches bundles without descending into them", "[vst3][scan]")
{
    TempDirectory temp ("dusk_vst3_scan_shape_");
    std::error_code ec;

    const auto bundle = temp.path() / "Fake.vst3";
#if defined(__APPLE__)
    const auto executableDir = bundle / "Contents/MacOS";
    const auto executable = executableDir / "Fake";
#else
    const auto executableDir = bundle / "Contents/x86_64-linux";
    const auto executable = executableDir / "Fake.so";
#endif
    REQUIRE (stdfs::create_directories (executableDir, ec));
    REQUIRE_FALSE (ec);
    REQUIRE (writeText (executable, "not a loadable module"));
    REQUIRE (writeText (executableDir / "Inner.vst3", "must not be discovered"));

    // Vendor subdirectory one level down.
    const auto nested = temp.path() / "Vendor/Nested.vst3";
    REQUIRE (stdfs::create_directories (nested.parent_path(), ec));
    REQUIRE_FALSE (ec);
    REQUIRE (writeText (nested, "not a loadable module"));

    const auto found = Vst3Scanner::findVst3Bundles ({ temp.path(), temp.path() });
    REQUIRE (found.size() == 2);
    bool sawBundle = false, sawNested = false;
    for (const auto& f : found)
    {
        REQUIRE (f.is_absolute());
        if (f == bundle) sawBundle = true;
        if (f == nested) sawNested = true;
    }
    REQUIRE (sawBundle);
    REQUIRE (sawNested);
}

TEST_CASE ("Vst3Scanner discovers and describes a real VST3 module", "[vst3][scan]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_VST3");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_VST3 not set — skipping live VST3-scan test");
        return;
    }

    std::error_code ec;
    auto bundle = stdfs::absolute (stdfs::u8path (path), ec).lexically_normal();
    if (! bundle.has_filename()) bundle = bundle.parent_path();
    REQUIRE_FALSE (ec);
    REQUIRE (stdfs::exists (bundle));

    const auto found = Vst3Scanner::scan ({ bundle.parent_path() });
    REQUIRE_FALSE (found.empty());

    bool sawBundle = false;
    for (const auto& s : found)
    {
        if (s.bundlePath == bundle.u8string())
        {
            sawBundle = true;
            REQUIRE_FALSE (s.desc.id.empty());     // a usable class id to instantiate
            REQUIRE_FALSE (s.desc.name.empty());   // a human-readable name for the picker
        }
    }
    REQUIRE (sawBundle);
}

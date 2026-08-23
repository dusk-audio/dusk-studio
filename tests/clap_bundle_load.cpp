// CLAP loader failure paths use only system libraries, except for the macOS
// directory-bundle resolver check's harmless no-clap_entry test dylib.

#include <catch2/catch_test_macros.hpp>

#include "engine/clap/ClapBundle.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
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

bool writeText (const stdfs::path& path, const char* text)
{
    std::ofstream stream (path);
    stream << text;
    return stream.good();
}
} // namespace
#endif

TEST_CASE ("ClapBundle fails gracefully on a missing bundle", "[clap][bundle]")
{
    duskstudio::clap::ClapBundle b;
    std::string err;

    REQUIRE_FALSE (b.load ("/nonexistent/path/definitely-not-a.clap", err));
    REQUIRE_FALSE (err.empty());          // a reason was reported
    REQUIRE_FALSE (b.isLoaded());
    REQUIRE (b.plugins().empty());
    REQUIRE (b.getFactory() == nullptr);
    REQUIRE (b.getPath().empty());
}

TEST_CASE ("ClapBundle rejects a real shared object that is not a CLAP bundle", "[clap][bundle]")
{
#if defined(_WIN32)
    // kernel32.dll loads fine but has no clap_entry, exercising the
    // post-LoadLibrary rejection path.
    wchar_t systemDir[MAX_PATH] {};
    REQUIRE (::GetSystemDirectoryW (systemDir, MAX_PATH) > 0);
    const auto library = std::filesystem::path (systemDir) / L"kernel32.dll";

    duskstudio::clap::ClapBundle b;
    std::string err;
    REQUIRE_FALSE (b.load (library.u8string(), err));
    REQUIRE_FALSE (b.isLoaded());
    REQUIRE (err == "no clap_entry symbol");
#else
    // A valid shared object that dlopens fine but has no clap_entry exercises the
    // post-dlopen rejection path. Resolve the host libc portably (its soname differs
    // across libc / OS) via dladdr instead of hardcoding libc.so.6.
    Dl_info info {};
    if (dladdr (reinterpret_cast<void*> (&std::printf), &info) == 0 || info.dli_fname == nullptr)
    {
        SUCCEED ("dladdr could not resolve a host shared object — skipping");
        return;
    }
    duskstudio::clap::ClapBundle b;
    std::string err;
    REQUIRE_FALSE (b.load (info.dli_fname, err));
    REQUIRE_FALSE (b.isLoaded());
    REQUIRE_FALSE (err.empty());
    INFO ("rejection reason: " << err);
#endif
}

#if defined(__APPLE__)
TEST_CASE ("ClapBundle resolves a directory bundle executable", "[clap][bundle]")
{
    TempDirectory parent ("dusk_clap_bundle_");
    const auto temp = parent.path() / "Test.clap";
    const auto contents = temp / "Contents";
    const auto executableDir = contents / "MacOS";
    std::error_code ec;
    REQUIRE (std::filesystem::create_directories (executableDir, ec));
    REQUIRE_FALSE (ec);
    REQUIRE (writeText (contents / "Info.plist",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>CFBundleExecutable</key><string>TestClap</string>\n"
        "<key>CFBundleIdentifier</key><string>com.duskaudio.test-clap</string>\n"
        "<key>CFBundlePackageType</key><string>BNDL</string>\n"
        "</dict></plist>\n"));

    const auto executable = executableDir / "TestClap";
    std::filesystem::copy_file (std::filesystem::u8path (DUSKSTUDIO_NON_CLAP_FIXTURE_PATH),
                                executable,
                                std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE (ec);

    duskstudio::clap::ClapBundle b;
    std::string err;
    REQUIRE_FALSE (b.load (temp.u8string(), err));
    INFO ("load error: " << err);
    // A successful resolution reaches the valid inner dylib and fails only
    // because the fixture deliberately exports no clap_entry symbol.
    REQUIRE (err == "no clap_entry symbol");
    REQUIRE_FALSE (b.isLoaded());
    REQUIRE (b.getPath().empty());
}
#endif

#include <catch2/catch_test_macros.hpp>

#include "foundation/Fs.h"
#include "session/RecentSessions.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dusk;
namespace stdfs = std::filesystem;

namespace
{
stdfs::path jucePath (juce::File::SpecialLocationType type)
{
    // toStdString() is UTF-8; u8path interprets it as such (path(std::string)
    // would use the active code page on Windows).
    return stdfs::u8path (juce::File::getSpecialLocation (type).getFullPathName().toStdString());
}

class ScopedTestSpecialLocations
{
public:
    explicit ScopedTestSpecialLocations (const stdfs::path& value)
    {
       #if defined(_WIN32)
        if (const wchar_t* existing = ::_wgetenv (kWideName))
        {
            hadPrevious = true;
            previous = existing;
        }
        if (::_wputenv_s (kWideName, value.c_str()) != 0)
            throw std::runtime_error ("could not set the special-location test override");
       #else
        if (const char* existing = std::getenv (kName))
        {
            hadPrevious = true;
            previous = existing;
        }
        const auto encoded = value.u8string();
        if (::setenv (kName, encoded.c_str(), 1) != 0)
            throw std::runtime_error ("could not set the special-location test override");
       #endif
    }

    ~ScopedTestSpecialLocations()
    {
       #if defined(_WIN32)
        ::_wputenv_s (kWideName, hadPrevious ? previous.c_str() : L"");
       #else
        if (hadPrevious) ::setenv (kName, previous.c_str(), 1);
        else             ::unsetenv (kName);
       #endif
    }

    ScopedTestSpecialLocations (const ScopedTestSpecialLocations&) = delete;
    ScopedTestSpecialLocations& operator= (const ScopedTestSpecialLocations&) = delete;
    ScopedTestSpecialLocations (ScopedTestSpecialLocations&&) = delete;
    ScopedTestSpecialLocations& operator= (ScopedTestSpecialLocations&&) = delete;

private:
    static constexpr const char* kName = "DUSKSTUDIO_TEST_SPECIAL_LOCATIONS_ROOT";
   #if defined(_WIN32)
    static constexpr const wchar_t* kWideName = L"DUSKSTUDIO_TEST_SPECIAL_LOCATIONS_ROOT";
    std::wstring previous {};
   #else
    std::string previous {};
   #endif
    bool hadPrevious = false;
};

#if defined(_WIN32)
class ScopedWideEnvironment
{
public:
    ScopedWideEnvironment (const wchar_t* nameIn, const stdfs::path& value)
        : name (nameIn)
    {
        if (const wchar_t* existing = ::_wgetenv (name.c_str()))
        {
            hadPrevious = true;
            previous = existing;
        }
        if (::_wputenv_s (name.c_str(), value.c_str()) != 0)
            throw std::runtime_error ("could not set a wide environment override");
    }

    ~ScopedWideEnvironment()
    {
        ::_wputenv_s (name.c_str(), hadPrevious ? previous.c_str() : L"");
    }

    ScopedWideEnvironment (const ScopedWideEnvironment&) = delete;
    ScopedWideEnvironment& operator= (const ScopedWideEnvironment&) = delete;
    ScopedWideEnvironment (ScopedWideEnvironment&&) = delete;
    ScopedWideEnvironment& operator= (ScopedWideEnvironment&&) = delete;

private:
    std::wstring name;
    std::wstring previous {};
    bool hadPrevious = false;
};
#endif
} // namespace

TEST_CASE ("dusk::fs special locations match juce::File", "[foundation][fs]")
{
    SECTION ("userHomeDir")
    {
        REQUIRE (fs::userHomeDir() == jucePath (juce::File::userHomeDirectory));
    }

    SECTION ("userConfigDir == userApplicationDataDirectory")
    {
        REQUIRE (fs::userConfigDir() == jucePath (juce::File::userApplicationDataDirectory));
    }

    SECTION ("userMusicDir")
    {
        REQUIRE (fs::userMusicDir() == jucePath (juce::File::userMusicDirectory));
    }

    SECTION ("tempDir")
    {
#if defined(__linux__) || defined(_WIN32)
        REQUIRE (fs::tempDir() == jucePath (juce::File::tempDirectory));
#else
        // JUCE's macOS tempDirectory has bespoke ~/Library/Caches/<exe>
        // semantics; dusk::fs deliberately follows $TMPDIR there.
        REQUIRE (std::filesystem::is_directory (fs::tempDir()));
#endif
    }

    SECTION ("createUniqueTempDirectory atomically claims distinct paths")
    {
        const auto first = fs::createUniqueTempDirectory ("dusk-fs-test-");
        const auto second = fs::createUniqueTempDirectory ("dusk-fs-test-");
        REQUIRE_FALSE (first.empty());
        REQUIRE_FALSE (second.empty());
        REQUIRE (first != second);
        REQUIRE (stdfs::is_directory (first));
        REQUIRE (stdfs::is_directory (second));

        std::error_code ec;
        stdfs::remove_all (first, ec);
        REQUIRE_FALSE (ec);
        stdfs::remove_all (second, ec);
        REQUIRE_FALSE (ec);
    }

    SECTION ("currentExecutablePath points at the same file as juce")
    {
        // JUCE resolves via dladdr (path relative to CWD); dusk reads /proc/self/exe.
        // Both name the running binary — compare canonical form.
        std::error_code ec1, ec2;
        const auto dusk = stdfs::canonical (fs::currentExecutablePath(), ec1);
        const auto juce = stdfs::canonical (jucePath (juce::File::currentExecutableFile), ec2);
        REQUIRE_FALSE (ec1);
        REQUIRE_FALSE (ec2);
        REQUIRE (dusk == juce);
    }
}


TEST_CASE ("Special locations preserve Unicode paths end to end",
           "[foundation][fs][windows][issue-379]")
{
    const auto root = stdfs::temp_directory_path()
                    / stdfs::u8path (u8"dusk-locations-\u97f3\u58f0")
                    / std::to_string ((long long) std::chrono::steady_clock::now()
                                           .time_since_epoch().count());
    std::error_code ec;
    stdfs::remove_all (root, ec);
    ec.clear();
    for (const auto* child : { "Home", "Config", "Music", "Temp" })
    {
        REQUIRE (stdfs::create_directories (root / child, ec));
        REQUIRE_FALSE (ec);
    }

   #if defined(_WIN32)
    REQUIRE (stdfs::create_directories (root / L"ApiTemp", ec));
    REQUIRE_FALSE (ec);
    {
        const ScopedWideEnvironment temp (L"TEMP", root / L"ApiTemp");
        const ScopedWideEnvironment tmp (L"TMP", root / L"ApiTemp");
        REQUIRE (fs::tempDir() == root / L"ApiTemp");
    }
   #endif

    {
        const ScopedTestSpecialLocations locations (root);
        REQUIRE (fs::userHomeDir() == root / "Home");
        REQUIRE (fs::userConfigDir() == root / "Config");
        REQUIRE (fs::userMusicDir() == root / "Music");
        REQUIRE (fs::tempDir() == root / "Temp");

        const auto session = root / "Home" / stdfs::u8path (u8"session-\u66f8\u304d\u51fa\u3057");
        REQUIRE (stdfs::create_directories (session, ec));
        REQUIRE_FALSE (ec);
        duskstudio::RecentSessions::add (session);
        REQUIRE (duskstudio::RecentSessions::load() == std::vector<stdfs::path> { session });
        REQUIRE (stdfs::is_regular_file (root / "Config" / "Dusk Studio" / "recent.txt"));

        const auto work = fs::createUniqueTempDirectory ("dusk-unicode-");
        REQUIRE_FALSE (work.empty());
        REQUIRE (work.parent_path() == root / "Temp");
        REQUIRE (fs::writeStringToFile (work / stdfs::u8path (u8"state-\u72b6\u614b.bin"), "state"));
        REQUIRE (fs::writeStringToFile (work / stdfs::u8path (u8"import-\u97f3\u58f0.wav"), "audio"));
        REQUIRE (fs::loadFileAsString (work / stdfs::u8path (u8"state-\u72b6\u614b.bin")) == "state");
        REQUIRE (fs::loadFileAsString (work / stdfs::u8path (u8"import-\u97f3\u58f0.wav")) == "audio");

        duskstudio::RecentSessions::clear();
    }

    stdfs::remove_all (root, ec);
    REQUIRE_FALSE (ec);
}

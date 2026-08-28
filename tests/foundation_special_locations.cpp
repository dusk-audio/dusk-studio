#include <catch2/catch_test_macros.hpp>

#include "foundation/Fs.h"

#include <juce_core/juce_core.h>

#include <filesystem>

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
#if defined(__linux__)
        REQUIRE (fs::tempDir() == jucePath (juce::File::tempDirectory));
#else
        // JUCE's macOS/Windows tempDirectory has bespoke semantics
        // (~/Library/Caches/<exe>, GetTempPath); dusk::fs::tempDir deliberately
        // follows the POSIX $TMPDIR/TMP convention. Temp files are ephemeral, so
        // this divergence is intentional — assert only that it is usable.
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

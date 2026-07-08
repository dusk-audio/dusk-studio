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
    return stdfs::path (juce::File::getSpecialLocation (type).getFullPathName().toStdString());
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
        REQUIRE (fs::tempDir() == jucePath (juce::File::tempDirectory));
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

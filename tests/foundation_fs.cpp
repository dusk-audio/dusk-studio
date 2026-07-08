#include <catch2/catch_test_macros.hpp>

#include "foundation/Fs.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <filesystem>
#include <random>
#include <set>
#include <string>

using namespace dusk;
namespace stdfs = std::filesystem;

namespace
{
std::set<std::string> juceNames (const stdfs::path& dir, const juce::String& wild, bool recursive)
{
    std::set<std::string> out;
    for (auto& f : juce::File (dir.string())
                       .findChildFiles (juce::File::findFiles, recursive, wild))
        out.insert (f.getFileName().toStdString());
    return out;
}

std::set<std::string> duskNames (const stdfs::path& dir, const std::string& wild, bool recursive)
{
    std::set<std::string> out;
    for (auto& p : fs::findChildFiles (dir, wild, recursive))
        out.insert (p.filename().string());
    return out;
}
} // namespace

TEST_CASE ("dusk::fs matches juce::File", "[foundation][fs]")
{
    const auto root = stdfs::temp_directory_path()
                          / ("dusk_fs_test_" + std::to_string (std::random_device {}()));
    stdfs::remove_all (root);
    stdfs::create_directories (root / "sub");

    REQUIRE (fs::writeStringToFile (root / "a.wav", "hello"));
    REQUIRE (fs::writeStringToFile (root / "b.wav", "world!!"));
    REQUIRE (fs::writeStringToFile (root / "c.txt", "text"));
    REQUIRE (fs::writeStringToFile (root / "sub" / "d.wav", "nested"));

    SECTION ("read + size interop with juce")
    {
        REQUIRE (fs::loadFileAsString (root / "a.wav") == "hello");
        REQUIRE (fs::loadFileAsString (root / "a.wav")
                 == juce::File ((root / "a.wav").string()).loadFileAsString().toStdString());
        REQUIRE (fs::fileSize (root / "b.wav")
                 == (std::int64_t) juce::File ((root / "b.wav").string()).getSize());
        REQUIRE (fs::fileSize (root / "missing") == 0);
    }

    SECTION ("hasExtension matches juce (case-insensitive, dot-tolerant)")
    {
        const auto p = root / "a.wav";
        const juce::File jf (p.string());
        for (const char* ext : { "wav", ".wav", "WAV", ".WAV", "txt" })
            REQUIRE (fs::hasExtension (p, ext) == jf.hasFileExtension (ext));
    }

    SECTION ("findChildFiles matches juce")
    {
        REQUIRE (duskNames (root, "*.wav", false) == juceNames (root, "*.wav", false));
        REQUIRE (duskNames (root, "*.wav", true)  == juceNames (root, "*.wav", true));
        REQUIRE (duskNames (root, "*",     false) == juceNames (root, "*",     false));
        REQUIRE (duskNames (root, "?.txt", false) == juceNames (root, "?.txt", false));
    }

    stdfs::remove_all (root);
}

TEST_CASE ("dusk::fs::matchesWildcard", "[foundation][fs]")
{
    REQUIRE (fs::matchesWildcard ("a.wav", "*.wav"));
    REQUIRE (fs::matchesWildcard ("A.WAV", "*.wav"));      // case-insensitive
    REQUIRE (fs::matchesWildcard ("x.txt", "?.txt"));
    REQUIRE_FALSE (fs::matchesWildcard ("xx.txt", "?.txt"));
    REQUIRE (fs::matchesWildcard ("anything", "*"));
    REQUIRE_FALSE (fs::matchesWildcard ("a.wav", "*.flac"));
}

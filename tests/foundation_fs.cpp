#include <catch2/catch_test_macros.hpp>

#include "foundation/Fs.h"

#include <juce_core/juce_core.h>

#include <filesystem>
#include <random>
#include <set>
#include <stdexcept>
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

class FailingDirectoryIterator
{
public:
    FailingDirectoryIterator() = default;
    explicit FailingDirectoryIterator (int* incrementsIn) : increments (incrementsIn), atEnd (false) {}

    int operator*() const noexcept { return 42; }

    FailingDirectoryIterator& operator++()
    {
        throw std::runtime_error ("throwing increment must not be used");
    }

    void increment (std::error_code& error)
    {
        ++*increments;
        error = std::make_error_code (std::errc::io_error);
    }

    friend bool operator!= (const FailingDirectoryIterator& lhs,
                            const FailingDirectoryIterator& rhs) noexcept
    {
        return lhs.atEnd != rhs.atEnd;
    }

private:
    int* increments = nullptr;
    bool atEnd = true;
};
} // namespace

TEST_CASE ("dusk::fs matches juce::File", "[foundation][fs]")
{
    const auto root = stdfs::temp_directory_path()
                          / ("dusk_fs_test_" + std::to_string (std::random_device {}()));
    stdfs::remove_all (root);
    stdfs::create_directories (root / "sub");

    auto writeError = std::make_error_code (std::errc::io_error);
    REQUIRE (fs::writeStringToFile (root / "a.wav", "hello", &writeError));
    CHECK_FALSE (writeError);
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

    SECTION ("failed writes retain their native error")
    {
        CHECK_FALSE (fs::writeStringToFile (root / "sub", "cannot replace a directory", &writeError));
        CHECK (writeError);
        CHECK (writeError != std::errc::no_space_on_device);
        CHECK (stdfs::is_directory (root / "sub"));
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

TEST_CASE ("native IO paths support long files without changing portable paths", "[foundation][fs]")
{
    CHECK (fs::nativeIOPath ({}).empty());
   #if defined(_WIN32)
    CHECK (fs::nativeIOPath (L"C:/library/./samples/../piano.sfz")
        == stdfs::path (LR"(\\?\C:\library\piano.sfz)"));
    CHECK (fs::nativeIOPath (LR"(\\server\share\piano.sfz)")
        == stdfs::path (LR"(\\?\UNC\server\share\piano.sfz)"));
    const stdfs::path extended (LR"(\\?\C:\library\piano.sfz)");
    CHECK (fs::nativeIOPath (extended) == extended);
    auto root = fs::createUniqueTempDirectory ("dusk-long-io-");
    const auto nativeRoot = fs::nativeIOPath (root);
    const auto removeRoot = [&nativeRoot] (void*)
    { std::error_code error; stdfs::remove_all (nativeRoot, error); };
    const std::unique_ptr<void, decltype (removeRoot)> cleanup (&root, removeRoot);
    const auto nested = nativeRoot / stdfs::u8path ("Samples/tone.wav");
    const auto expected = nativeRoot / L"Samples" / L"tone.wav";
    CHECK (fs::nativeIOPath (nested).native() == expected.native());
    CHECK (fs::nativeIOPath (LR"(\\?\UNC\server\share\Samples/tone.wav)").native()
        == LR"(\\?\UNC\server\share\Samples\tone.wav)");
    REQUIRE (stdfs::create_directory (nativeRoot / L"Samples"));
    REQUIRE (fs::writeStringToFile (nested, "nested sample"));
    CHECK (fs::loadFileAsString (nested) == "nested sample");
    auto directory = root;
    for (int index = 0; index < 4; ++index) directory /= std::wstring (72, wchar_t (L'a' + index));
    REQUIRE (directory.native().size() > 260);
    REQUIRE (stdfs::create_directories (fs::nativeIOPath (directory)));
    const auto file = directory / stdfs::u8path (u8"音色.sfz");
    REQUIRE (fs::writeStringToFile (file, "long-path instrument"));
    CHECK (fs::loadFileAsString (file) == "long-path instrument");
    CHECK (fs::fileSize (file) == 20);
    CHECK (file.native().compare (0, 4, L"\\\\?\\") != 0);
   #else
    const stdfs::path original ("alias/../library/piano.sfz");
    CHECK (fs::nativeIOPath (original).native() == original.native());
   #endif
}

TEST_CASE ("directory walks report increment errors without throwing", "[foundation][fs][issue-374]")
{
    std::error_code error;
    int increments = 0;
    int visits = 0;
    int visitedValue = 0;
    bool completed = true;

    REQUIRE_NOTHROW (completed = fs::walkDirectoryEntries (
                         FailingDirectoryIterator (&increments), error,
                         [&] (int value)
                         {
                             ++visits;
                             visitedValue = value;
                             return true;
                         }));

    REQUIRE_FALSE (completed);
    REQUIRE (error == std::errc::io_error);
    REQUIRE (visits == 1);
    REQUIRE (visitedValue == 42);
    REQUIRE (increments == 1);
}

#include <catch2/catch_test_macros.hpp>

#include "foundation/Fs.h"
#include "ui/DroppedFiles.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
namespace stdfs = std::filesystem;
using duskstudio::droppedfiles::isImportableFile;

struct DropFolder
{
    DropFolder() : root (dusk::fs::createUniqueTempDirectory ("dusk-dropped-files-"))
    {
        for (const char* name : { "take.wav", "Loud Take.WAV", "song.mid", "notes.txt", "dr\xc3\xb6p.flac" })
            std::ofstream (root / stdfs::u8path (name)) << "x";
        stdfs::create_directories (root / "folder.wav");
    }
    ~DropFolder()
    {
        std::error_code ec;
        stdfs::remove_all (root, ec);
    }

    std::string at (const char* name) const { return (root / stdfs::u8path (name)).u8string(); }

    stdfs::path root;
};
} // namespace

TEST_CASE ("A drop keeps only existing local files the tape strip can import")
{
    DropFolder drop;

    SECTION ("existing audio and MIDI files are kept, whatever the extension's case")
    {
        CHECK (isImportableFile (drop.at ("take.wav")));
        CHECK (isImportableFile (drop.at ("Loud Take.WAV")));
        CHECK (isImportableFile (drop.at ("song.mid")));
        CHECK (isImportableFile (drop.at ("dr\xc3\xb6p.flac")));
    }

    SECTION ("a web link that ends in an audio extension is not a file")
    {
        CHECK_FALSE (isImportableFile ("https://example.com/take.wav"));
        CHECK_FALSE (isImportableFile ("file:///tmp/take.wav"));
    }

    SECTION ("missing files, folders, relative paths and other types are dropped")
    {
        CHECK_FALSE (isImportableFile (drop.at ("gone.wav")));
        CHECK_FALSE (isImportableFile (drop.at ("folder.wav")));
        CHECK_FALSE (isImportableFile (drop.at ("notes.txt")));
        CHECK_FALSE (isImportableFile ("take.wav"));
        CHECK_FALSE (isImportableFile ("localhost" + drop.at ("take.wav")));
        CHECK_FALSE (isImportableFile (""));
    }
}

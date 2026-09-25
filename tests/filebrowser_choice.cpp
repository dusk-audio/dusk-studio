#include <catch2/catch_test_macros.hpp>

#include "foundation/Fs.h"
#include "ui/FileBrowserChoice.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
namespace stdfs = std::filesystem;
using duskstudio::filebrowser::Mode;
using duskstudio::filebrowser::isAcceptableChoice;

struct ScratchFolder
{
    ScratchFolder() : dir (dusk::fs::createUniqueTempDirectory ("dusk-filebrowser-choice-"))
    {
        std::ofstream (dir / "take.wav") << "x";
    }
    ~ScratchFolder()
    {
        std::error_code ec;
        stdfs::remove_all (dir, ec);
    }
    std::string path (const char* child) const { return (dir / child).u8string(); }
    std::string folder() const { return dir.u8string(); }

    stdfs::path dir;
};
} // namespace

TEST_CASE ("file browser Open hands back only an existing file")
{
    const ScratchFolder scratch;
    REQUIRE_FALSE (scratch.dir.empty());

    CHECK (isAcceptableChoice (scratch.path ("take.wav"), Mode::Open, false, false));
    CHECK_FALSE (isAcceptableChoice (scratch.folder(), Mode::Open, false, false));
    CHECK_FALSE (isAcceptableChoice (scratch.path ("Music"), Mode::Open, false, false));
    CHECK_FALSE (isAcceptableChoice ({}, Mode::Open, false, false));
}

TEST_CASE ("file browser Save and folder pickers keep the browser's answer")
{
    const ScratchFolder scratch;
    REQUIRE_FALSE (scratch.dir.empty());

    SECTION ("Save takes a name that does not exist yet")
    {
        CHECK (isAcceptableChoice (scratch.path ("new mix.wav"), Mode::Save, false, false));
        CHECK (isAcceptableChoice (scratch.path ("take.wav"), Mode::Save, false, false));
        CHECK_FALSE (isAcceptableChoice ({}, Mode::Save, false, false));
    }

    SECTION ("a folder picker takes the folder")
    {
        CHECK (isAcceptableChoice (scratch.folder(), Mode::Open, true, false));
        CHECK_FALSE (isAcceptableChoice ({}, Mode::Open, true, false));
    }
}

TEST_CASE ("file browser Save never hands back a folder as a file name")
{
    const ScratchFolder scratch;
    REQUIRE_FALSE (scratch.dir.empty());
    stdfs::create_directory (scratch.dir / "Mixes");

    SECTION ("a file save refuses the browsed folder and a folder inside it")
    {
        CHECK_FALSE (isAcceptableChoice (scratch.folder(), Mode::Save, false, false));
        CHECK_FALSE (isAcceptableChoice (scratch.path ("Mixes"), Mode::Save, false, false));
        CHECK (isAcceptableChoice (scratch.path ("Mixes.wav"), Mode::Save, false, false));
    }

    SECTION ("a save whose answer is a folder takes an existing one or a new name")
    {
        CHECK (isAcceptableChoice (scratch.folder(), Mode::Save, false, true));
        CHECK (isAcceptableChoice (scratch.path ("Mixes"), Mode::Save, false, true));
        CHECK (isAcceptableChoice (scratch.path ("MySong"), Mode::Save, false, true));
        CHECK_FALSE (isAcceptableChoice ({}, Mode::Save, false, true));
    }
}

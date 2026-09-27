#include <catch2/catch_test_macros.hpp>

#include "foundation/Fs.h"
#include "ui/SaveTargetChecks.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
namespace stdfs = std::filesystem;
using duskstudio::savecheck::holdsAnotherSession;

struct ScratchSessions
{
    ScratchSessions() : root (dusk::fs::createUniqueTempDirectory ("dusk-save-target-"))
    {
        stdfs::create_directories (mine);
        stdfs::create_directories (other);
        stdfs::create_directories (empty);
        std::ofstream (mine / "session.json") << "{}";
        std::ofstream (other / "session.json") << "{}";
    }
    ~ScratchSessions()
    {
        std::error_code ec;
        stdfs::remove_all (root, ec);
    }

    stdfs::path root;
    stdfs::path mine  = root / "Mine";
    stdfs::path other = root / "Other";
    stdfs::path empty = root / "Empty";
};
} // namespace

TEST_CASE ("Save As counts only a different folder holding session.json as another session")
{
    const ScratchSessions s;
    REQUIRE_FALSE (s.root.empty());

    CHECK (holdsAnotherSession (s.other, s.mine));
    CHECK (holdsAnotherSession (s.other, {}));
    CHECK (holdsAnotherSession (s.other, s.root / "Gone"));

    CHECK_FALSE (holdsAnotherSession (s.mine, s.mine));
    CHECK_FALSE (holdsAnotherSession (s.mine / ".", s.mine));
    CHECK_FALSE (holdsAnotherSession (s.root / "Other" / ".." / "Mine", s.mine));
    CHECK_FALSE (holdsAnotherSession (s.empty, s.mine));
    CHECK_FALSE (holdsAnotherSession (s.root / "New", s.mine));

    std::error_code ec;
    stdfs::create_directory_symlink (s.mine, s.root / "Link", ec);
    if (! ec)
        CHECK_FALSE (holdsAnotherSession (s.root / "Link", s.mine));
}

TEST_CASE ("Save As does not count a folder named session.json as a session")
{
    const ScratchSessions s;
    REQUIRE_FALSE (s.root.empty());
    stdfs::create_directories (s.empty / "session.json");

    CHECK_FALSE (holdsAnotherSession (s.empty, s.mine));
}

TEST_CASE ("the replace prompt names the file and adds a note only when given one")
{
    using duskstudio::savecheck::replaceFileMessage;

    CHECK (replaceFileMessage ("mix.wav")
           == "This file already exists and will be replaced:\n\nmix.wav\n\nContinue?");
    CHECK (replaceFileMessage ("mix.wav", "Realtime bounces are always written as WAV.")
           == "This file already exists and will be replaced:\n\nmix.wav\n\n"
              "Realtime bounces are always written as WAV.\n\nContinue?");
}

TEST_CASE ("Save As treats another spelling of the session's own folder as the same folder")
{
    using duskstudio::savecheck::isSameLocation;
    const ScratchSessions s;
    REQUIRE_FALSE (s.root.empty());

    CHECK (isSameLocation (s.mine, s.mine));
    CHECK (isSameLocation (s.root / "Other" / ".." / "Mine", s.mine));
    CHECK_FALSE (isSameLocation (s.other, s.mine));
    CHECK_FALSE (isSameLocation (s.root / "New", s.mine));
    CHECK_FALSE (isSameLocation (s.mine, {}));

    std::error_code ec;
    stdfs::create_directory_symlink (s.mine, s.root / "Link", ec);
    if (! ec)
        CHECK (isSameLocation (s.root / "Link", s.mine));
}

TEST_CASE ("Export master knows the loaded mix under any name that reaches it")
{
    using duskstudio::savecheck::isSameLocation;
    const ScratchSessions s;
    REQUIRE_FALSE (s.root.empty());
    const auto mix = s.mine / "mixdown.wav";
    std::ofstream (mix, std::ios::binary) << "mix";
    std::ofstream (s.mine / "master.wav", std::ios::binary) << "master";

    CHECK (isSameLocation (mix, mix));
    CHECK (isSameLocation (s.other / ".." / "Mine" / "mixdown.wav", mix));
    CHECK_FALSE (isSameLocation (s.mine / "master.wav", mix));
    CHECK_FALSE (isSameLocation (s.mine / "master2.wav", mix));
    CHECK_FALSE (isSameLocation (s.mine / "mixdown.wav", {}));

    std::error_code ec;
    const auto relative = stdfs::relative (mix, ec);
    if (! ec && ! relative.empty() && relative.is_relative())
        CHECK (isSameLocation (relative, mix));

    stdfs::create_symlink (mix, s.other / "link.wav", ec);
    if (! ec)
        CHECK (isSameLocation (s.other / "link.wav", mix));

    stdfs::create_hard_link (mix, s.other / "hard.wav", ec);
    if (! ec)
        CHECK (isSameLocation (s.other / "hard.wav", mix));

    // Where the filesystem ignores case, the other spelling opens the mix.
    const auto upper = s.mine / "MIXDOWN.wav";
    CHECK (isSameLocation (upper, mix) == stdfs::exists (upper, ec));
}

TEST_CASE ("the loaded-mix refusal names the file and says nothing was written")
{
    using duskstudio::savecheck::loadedMixMessage;

    CHECK (loadedMixMessage ("mixdown.wav")
           == "This file is the mix the master is rendered from:\n\n    mixdown.wav\n\n"
              "Exporting over it would destroy the mix, so nothing was exported and nothing was changed. "
              "Choose another name for the master.");
}

TEST_CASE ("a never-saved session keeps its autosave and notes out of a folder holding a session")
{
    using duskstudio::savecheck::sidecarFolder;
    const ScratchSessions s;
    REQUIRE_FALSE (s.root.empty());
    const auto privateDir = s.root / "private";

    CHECK (sidecarFolder (s.other, false, privateDir) == privateDir);
    CHECK (sidecarFolder (s.other, false, {}).empty());
    CHECK (sidecarFolder (s.empty, false, privateDir) == s.empty);
    CHECK (sidecarFolder (s.root / "New", false, privateDir) == s.root / "New");
    CHECK (sidecarFolder (s.mine, true, privateDir) == s.mine);
    CHECK (sidecarFolder ({}, false, privateDir).empty());

    stdfs::create_directories (s.empty / "session.json");
    CHECK (sidecarFolder (s.empty, false, privateDir) == privateDir);

    std::error_code ec;
    stdfs::create_directories (s.root / "Linked");
    stdfs::create_symlink (s.root / "Gone" / "session.json", s.root / "Linked" / "session.json", ec);
    if (! ec)
        CHECK (sidecarFolder (s.root / "Linked", false, privateDir) == privateDir);
}

TEST_CASE ("a never-saved session starts in the first Untitled folder that holds no session")
{
    using duskstudio::savecheck::unsavedSessionFolder;
    const ScratchSessions s;
    REQUIRE_FALSE (s.root.empty());

    CHECK (unsavedSessionFolder (s.root) == s.root / "Untitled");

    stdfs::create_directories (s.root / "Untitled");
    std::ofstream (s.root / "Untitled" / "notepad.md") << "notes";
    CHECK (unsavedSessionFolder (s.root) == s.root / "Untitled");

    std::ofstream (s.root / "Untitled" / "session.json") << "{}";
    CHECK (unsavedSessionFolder (s.root) == s.root / "Untitled 2");

    stdfs::create_directories (s.root / "Untitled 2" / "session.json");
    CHECK (unsavedSessionFolder (s.root) == s.root / "Untitled 3");
}

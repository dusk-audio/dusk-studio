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
    using duskstudio::savecheck::isSameFolder;
    const ScratchSessions s;
    REQUIRE_FALSE (s.root.empty());

    CHECK (isSameFolder (s.mine, s.mine));
    CHECK (isSameFolder (s.root / "Other" / ".." / "Mine", s.mine));
    CHECK_FALSE (isSameFolder (s.other, s.mine));
    CHECK_FALSE (isSameFolder (s.root / "New", s.mine));
    CHECK_FALSE (isSameFolder (s.mine, {}));

    std::error_code ec;
    stdfs::create_directory_symlink (s.mine, s.root / "Link", ec);
    if (! ec)
        CHECK (isSameFolder (s.root / "Link", s.mine));
}

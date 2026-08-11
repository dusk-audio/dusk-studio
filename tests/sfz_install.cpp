#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzDownload.h"
#include "engine/sfz/SfzInstall.h"
#include "sfz_pack_fixture.h"

#include <string>
#include <vector>

namespace
{
using duskstudio::sfz::InstallStatus;
using duskstudio::sfz::findUnsafeSfzReference;
using duskstudio::sfz::installPack;
using sfzfixture::PackFixture;
using sfzfixture::kInstrumentText;
} // namespace

TEST_CASE ("SFZ install publishes a verified pack", "[sfz][install]")
{
    PackFixture fixture;
    const auto result = fixture.install();

    REQUIRE (result.status == InstallStatus::installed);
    CHECK (result.installedPath == fixture.layout.packDirectory ("pack", "v1.0.0"));
    CHECK (std::filesystem::is_regular_file (result.installedPath / "Kit.sfz"));
    CHECK (std::filesystem::is_regular_file (result.installedPath / "Samples/kick.wav"));
    CHECK (fixture.stagingIsClean());
    // The verified archive is only scratch space once the pack is published.
    CHECK (std::filesystem::is_empty (fixture.layout.downloadsDirectory()));
}

TEST_CASE ("SFZ install leaves an existing pack alone", "[sfz][install]")
{
    PackFixture fixture;
    REQUIRE (fixture.install().status == InstallStatus::installed);
    const auto attemptsAfterFirst = fixture.transport.attempts;

    const auto again = fixture.install();
    CHECK (again.status == InstallStatus::alreadyInstalled);
    CHECK (fixture.transport.attempts == attemptsAfterFirst);
}

TEST_CASE ("SFZ install refuses a withdrawn pack", "[sfz][install]")
{
    PackFixture fixture;
    auto pack = fixture.pack();
    pack.yanked = true;

    const auto result = installPack (fixture.transport, pack, fixture.layout, {}, {});
    CHECK (result.status == InstallStatus::packRejected);
    CHECK (fixture.transport.attempts == 0);
}

TEST_CASE ("SFZ install refuses pack metadata it would turn into a path", "[sfz][install]")
{
    PackFixture fixture;

    const auto rejects = [&fixture] (duskstudio::sfz::CatalogPack pack)
    {
        const auto result = installPack (fixture.transport, pack, fixture.layout, {}, {});
        CHECK (result.status == InstallStatus::packRejected);
        CHECK (fixture.transport.attempts == 0);
    };

    SECTION ("traversal in the pack id")
    {
        auto pack = fixture.pack();
        pack.id = "..";
        rejects (pack);
    }

    SECTION ("separator in the release id")
    {
        auto pack = fixture.pack();
        pack.releaseId = "v1/../../etc";
        rejects (pack);
    }

    SECTION ("traversal in the declared license path")
    {
        auto pack = fixture.pack();
        pack.license.file = "../../LICENSE";
        rejects (pack);
    }

    SECTION ("traversal in an instrument entrypoint")
    {
        auto pack = fixture.pack();
        pack.instruments.front().relativeEntrypoint = "../../Kit.sfz";
        rejects (pack);
    }

    CHECK_FALSE (std::filesystem::exists (fixture.layout.packsDirectory()));
}

TEST_CASE ("SFZ install refuses an archive that fails its digest", "[sfz][install]")
{
    PackFixture fixture;
    auto pack = fixture.pack();
    pack.archiveSha256 = std::string (64, 'c');

    const auto result = installPack (fixture.transport, pack, fixture.layout, {}, {});
    CHECK (result.status == InstallStatus::downloadFailed);
    CHECK_FALSE (std::filesystem::exists (fixture.layout.packsDirectory()));
    CHECK (fixture.stagingIsClean());
}

TEST_CASE ("SFZ install refuses a malicious archive", "[sfz][install]")
{
    PackFixture fixture;

    SECTION ("traversal entry")
    {
        fixture.entries.push_back (sfzfixture::file ("pack/../escape.wav", "x"));
        const auto result = fixture.install();
        CHECK (result.status == InstallStatus::archiveRejected);
    }

    SECTION ("symlink entry")
    {
        fixture.entries.push_back (sfzfixture::symlink ("pack/escape", "/etc/passwd"));
        const auto result = fixture.install();
        CHECK (result.status == InstallStatus::archiveRejected);
    }

    SECTION ("more files than the catalog declares")
    {
        auto pack = fixture.pack();
        pack.maxFiles = 2;
        const auto result = installPack (fixture.transport, pack, fixture.layout, {}, {});
        CHECK (result.status == InstallStatus::archiveRejected);
    }

    CHECK_FALSE (std::filesystem::exists (fixture.layout.packsDirectory() / "pack"));
    CHECK (fixture.stagingIsClean());
}

TEST_CASE ("SFZ install validates the pack against the catalog", "[sfz][install]")
{
    PackFixture fixture;

    SECTION ("the declared license file is missing")
    {
        fixture.entries.erase (fixture.entries.begin() + 1);
        CHECK (fixture.install().status == InstallStatus::validationFailed);
    }

    SECTION ("the license file was substituted")
    {
        fixture.entries[1] = sfzfixture::file ("pack/LICENSE", "All rights reserved\n");
        CHECK (fixture.install().status == InstallStatus::validationFailed);
    }

    SECTION ("an instrument entrypoint is missing")
    {
        fixture.entries[2] = sfzfixture::file ("pack/Other.sfz", kInstrumentText);
        CHECK (fixture.install().status == InstallStatus::validationFailed);
    }

    SECTION ("an instrument reaches outside the pack")
    {
        fixture.entries[2] = sfzfixture::file ("pack/Kit.sfz",
                                               "<region> sample=../../../etc/passwd\n");
        const auto result = fixture.install();
        CHECK (result.status == InstallStatus::validationFailed);
        CHECK (result.error.find ("outside the pack") != std::string::npos);
    }

    SECTION ("the archive is missing the expected root folder")
    {
        fixture.entries = { sfzfixture::file ("pack/Kit.sfz", kInstrumentText) };
        auto pack = fixture.pack();
        pack.expectedRoot = "pack";
        pack.license.file = "LICENSE";
        // The root exists but the license does not, so validation, not
        // extraction, is what refuses the pack.
        CHECK (installPack (fixture.transport, pack, fixture.layout, {}, {}).status
               == InstallStatus::validationFailed);
    }

    CHECK_FALSE (std::filesystem::exists (fixture.layout.packsDirectory() / "pack"));
    CHECK (fixture.stagingIsClean());
}

TEST_CASE ("SFZ install stops when cancelled", "[sfz][install]")
{
    PackFixture fixture;
    bool cancelled = false;
    duskstudio::sfz::InstallCallbacks callbacks;
    callbacks.isCancelled = [&cancelled] { return cancelled; };
    fixture.transport.chunkBytes = 16;
    fixture.transport.onChunkDelivered = [&cancelled] (std::uint64_t) { cancelled = true; };

    const auto result = fixture.install (callbacks);

    CHECK (result.status == InstallStatus::cancelled);
    CHECK_FALSE (std::filesystem::exists (fixture.layout.packsDirectory() / "pack"));
    CHECK (fixture.stagingIsClean());
}

TEST_CASE ("SFZ instrument references stay inside the pack", "[sfz][install]")
{
    sfzfixture::TempDirectory temp;
    const auto instrument = temp.path / "Kit.sfz";

    const auto scan = [&instrument] (const std::string& text)
    {
        sfzfixture::writeBinaryFile (instrument, text);
        return findUnsafeSfzReference (instrument);
    };

    SECTION ("ordinary references are accepted")
    {
        CHECK (scan ("<region> sample=Samples/kick.wav\n").empty());
        CHECK (scan ("<control> default_path=Samples/\n").empty());
        CHECK (scan ("#include \"parts/keys.sfz\"\n").empty());
        // Windows-authored packs separate with backslashes.
        CHECK (scan ("<region> sample=Samples\\kick.wav\n").empty());
        // Built-in generators are names, not paths.
        CHECK (scan ("<region> sample=*saw\n").empty());
        // A trailing opcode must not be swallowed into the sample path.
        CHECK (scan ("<region> sample=Samples/kick.wav lokey=36 hikey=36\n").empty());
        CHECK (scan ("// sample=../../etc/passwd\n").empty());
        CHECK (scan ("#define $KIT Samples\n<region> sample=$KIT/kick.wav\n").empty());
        CHECK (scan ("#define $LOKEY 36\n").empty());
    }

    SECTION ("escaping references are refused")
    {
        CHECK_FALSE (scan ("<region> sample=../../etc/passwd\n").empty());
        CHECK_FALSE (scan ("<region> sample=/etc/passwd\n").empty());
        CHECK_FALSE (scan ("<region> sample=C:\\Windows\\win.ini\n").empty());
        CHECK_FALSE (scan ("<region> sample=\\\\server\\share\\kick.wav\n").empty());
        CHECK_FALSE (scan ("<control> default_path=../../\n").empty());
        CHECK_FALSE (scan ("#include \"../../secrets.sfz\"\n").empty());
        CHECK_FALSE (scan ("<region> sample=Samples/../../kick.wav lokey=36\n").empty());
        // A variable is expanded into the sample path, so it cannot smuggle
        // traversal past the reference check.
        CHECK_FALSE (scan ("#define $KIT ../..\n<region> sample=$KIT/etc/passwd\n").empty());
        CHECK_FALSE (scan ("#define $KIT /etc\n").empty());
    }
}

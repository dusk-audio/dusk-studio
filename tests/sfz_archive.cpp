#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzArchive.h"
#include "engine/sfz/SfzArchivePolicy.h"
#include "sfz_pack_fixture.h"

#include <string>
#include <vector>

namespace
{
using duskstudio::sfz::ArchiveEntry;
using duskstudio::sfz::ArchiveEntryKind;
using duskstudio::sfz::ArchiveLimits;
using duskstudio::sfz::ArchivePolicy;
using duskstudio::sfz::ExtractionRequest;
using duskstudio::sfz::ExtractionStatus;
using duskstudio::sfz::extractZipArchive;

ArchiveLimits defaultLimits()
{
    ArchiveLimits limits;
    limits.expectedRoot = "pack";
    limits.maximumExpandedBytes = 1024 * 1024;
    limits.maximumFileBytes = 64 * 1024;
    limits.maximumEntries = 32;
    return limits;
}

ArchiveEntry regularFile (std::string name, std::uint64_t size)
{
    return { std::move (name), ArchiveEntryKind::regularFile, size, true };
}

ArchiveEntry directoryEntry (std::string name)
{
    return { std::move (name), ArchiveEntryKind::directory, 0, false };
}

void checkRejected (const std::vector<ArchiveEntry>& entries, const std::string& reason)
{
    ArchivePolicy policy (defaultLimits());
    bool rejected = false;
    for (const auto& entry : entries)
        if (! policy.accept (entry))
        {
            rejected = true;
            break;
        }
    CHECK (rejected);
    CHECK (policy.error().find (reason) != std::string::npos);
}

duskstudio::sfz::ExtractionResult extract (const sfzfixture::TempDirectory& temp,
                                           const std::vector<sfzfixture::ZipEntry>& entries,
                                           ArchiveLimits limits = defaultLimits())
{
    const auto archive = temp.path / "pack.zip";
    sfzfixture::writeBinaryFile (archive, sfzfixture::buildZip (entries));

    const auto destination = temp.path / "expand";
    std::filesystem::create_directories (destination);

    ExtractionRequest request;
    request.archiveFile = archive;
    request.destinationDirectory = destination;
    request.limits = std::move (limits);
    return extractZipArchive (request, {});
}
} // namespace

TEST_CASE ("SFZ archive policy accepts a well-formed pack", "[sfz][archive]")
{
    ArchivePolicy policy (defaultLimits());
    REQUIRE (policy.accept (directoryEntry ("pack/")));
    REQUIRE (policy.acceptedPath() == "pack");
    REQUIRE (policy.accept (directoryEntry ("pack/Samples/")));
    REQUIRE (policy.accept (regularFile ("pack/Samples/kick.wav", 128)));
    REQUIRE (policy.addFileBytes (128));
    REQUIRE (policy.finish());
    CHECK (policy.fileCount() == 1);
    CHECK (policy.expandedBytes() == 128);
}

TEST_CASE ("SFZ archive policy refuses unsafe entry names", "[sfz][archive]")
{
    SECTION ("parent traversal")
    {
        checkRejected ({ regularFile ("pack/../escape.wav", 4) }, "dot path segments");
    }

    SECTION ("traversal that resolves back inside")
    {
        checkRejected ({ regularFile ("pack/samples/../kick.wav", 4) }, "dot path segments");
    }

    SECTION ("absolute path")
    {
        checkRejected ({ regularFile ("/etc/passwd", 4) }, "forward-slash relative path");
    }

    SECTION ("windows drive letter")
    {
        checkRejected ({ regularFile ("C:/pack/kick.wav", 4) }, "Win32-invalid");
    }

    SECTION ("UNC path")
    {
        checkRejected ({ regularFile ("\\\\server\\share\\kick.wav", 4) },
                       "forward-slash relative path");
    }

    SECTION ("backslash separator")
    {
        checkRejected ({ regularFile ("pack\\kick.wav", 4) }, "forward-slash relative path");
    }

    SECTION ("outside the expected root")
    {
        checkRejected ({ regularFile ("other/kick.wav", 4) }, "outside the pack root");
    }

    SECTION ("a bare file where the pack root belongs")
    {
        checkRejected ({ regularFile ("pack", 4) }, "pack root must be a directory");
    }

    SECTION ("reserved windows device name")
    {
        checkRejected ({ regularFile ("pack/NUL.wav", 4) }, "reserved Windows device");
    }

    SECTION ("control character")
    {
        checkRejected ({ regularFile (std::string ("pack/ki\x01" "ck.wav"), 4) },
                       "control character");
    }

    SECTION ("links and devices")
    {
        checkRejected ({ ArchiveEntry { "pack/link.wav", ArchiveEntryKind::unsupported, 0, false } },
                       "not a file or directory");
    }
}

TEST_CASE ("SFZ archive policy refuses colliding names", "[sfz][archive]")
{
    SECTION ("exact duplicate file")
    {
        checkRejected ({ regularFile ("pack/kick.wav", 4), regularFile ("pack/kick.wav", 4) },
                       "duplicate or case-colliding");
    }

    SECTION ("case-only difference")
    {
        checkRejected ({ regularFile ("pack/kick.wav", 4), regularFile ("pack/Kick.WAV", 4) },
                       "duplicate or case-colliding");
    }

    SECTION ("file where a directory already exists")
    {
        checkRejected ({ regularFile ("pack/samples/kick.wav", 4), regularFile ("pack/samples", 4) },
                       "duplicate or case-colliding");
    }

    SECTION ("directory where a file already exists")
    {
        checkRejected ({ regularFile ("pack/samples", 4), regularFile ("pack/samples/kick.wav", 4) },
                       "collides with a file name");
    }

    SECTION ("a repeated directory entry is not a collision")
    {
        ArchivePolicy policy (defaultLimits());
        REQUIRE (policy.accept (directoryEntry ("pack/samples/")));
        REQUIRE (policy.accept (directoryEntry ("pack/samples/")));
    }
}

TEST_CASE ("SFZ archive policy enforces the catalog budgets", "[sfz][archive]")
{
    SECTION ("entry count")
    {
        auto limits = defaultLimits();
        limits.maximumEntries = 2;
        ArchivePolicy policy (limits);
        REQUIRE (policy.accept (directoryEntry ("pack/")));
        REQUIRE (policy.accept (regularFile ("pack/one.wav", 4)));
        REQUIRE_FALSE (policy.accept (regularFile ("pack/two.wav", 4)));
        CHECK (policy.error().find ("more entries") != std::string::npos);
    }

    SECTION ("declared single-file size")
    {
        checkRejected ({ regularFile ("pack/huge.wav", 64 * 1024 + 1) },
                       "larger than the catalog allows");
    }

    SECTION ("written bytes beat an understated header")
    {
        auto limits = defaultLimits();
        limits.maximumFileBytes = 8;
        ArchivePolicy policy (limits);
        REQUIRE (policy.accept (ArchiveEntry { "pack/lie.wav", ArchiveEntryKind::regularFile,
                                               0, false }));
        REQUIRE (policy.addFileBytes (8));
        REQUIRE_FALSE (policy.addFileBytes (1));
        CHECK (policy.error().find ("larger than the catalog allows") != std::string::npos);
    }

    SECTION ("whole-archive expansion")
    {
        auto limits = defaultLimits();
        limits.maximumExpandedBytes = 16;
        ArchivePolicy policy (limits);
        REQUIRE (policy.accept (regularFile ("pack/a.wav", 8)));
        REQUIRE (policy.addFileBytes (8));
        REQUIRE (policy.accept (regularFile ("pack/b.wav", 8)));
        REQUIRE (policy.addFileBytes (8));
        REQUIRE (policy.accept (ArchiveEntry { "pack/c.wav", ArchiveEntryKind::regularFile,
                                               0, false }));
        REQUIRE_FALSE (policy.addFileBytes (1));
        CHECK (policy.error().find ("more bytes than the catalog allows") != std::string::npos);
    }

    SECTION ("an archive with no files at all")
    {
        ArchivePolicy policy (defaultLimits());
        REQUIRE (policy.accept (directoryEntry ("pack/")));
        REQUIRE_FALSE (policy.finish());
        CHECK (policy.error().find ("no files") != std::string::npos);
    }
}

TEST_CASE ("SFZ extraction writes a well-formed pack", "[sfz][archive]")
{
    sfzfixture::TempDirectory temp;
    const std::string instrument = "<region> sample=Samples/kick.wav\n";
    const auto result = extract (temp, {
        sfzfixture::directory ("pack"),
        sfzfixture::directory ("pack/Samples"),
        sfzfixture::file ("pack/Instrument.sfz", instrument),
        sfzfixture::file ("pack/Samples/kick.wav", std::string (200, 'k')) });

    REQUIRE (result.status == ExtractionStatus::completed);
    CHECK (result.fileCount == 2);
    CHECK (result.expandedBytes == 200 + instrument.size());
    CHECK (std::filesystem::is_regular_file (temp.path / "expand/pack/Samples/kick.wav"));
    CHECK (sfzfixture::readBinaryFile (temp.path / "expand/pack/Samples/kick.wav")
           == std::string (200, 'k'));
}

TEST_CASE ("SFZ extraction creates missing parent directories", "[sfz][archive]")
{
    sfzfixture::TempDirectory temp;
    const auto result = extract (temp, {
        sfzfixture::file ("pack/deep/nested/kick.wav", "data") });

    REQUIRE (result.status == ExtractionStatus::completed);
    CHECK (std::filesystem::is_regular_file (temp.path / "expand/pack/deep/nested/kick.wav"));
}

TEST_CASE ("SFZ extraction rejects malicious archives", "[sfz][archive]")
{
    SECTION ("traversal escapes the destination")
    {
        sfzfixture::TempDirectory temp;
        const auto result = extract (temp, {
            sfzfixture::file ("pack/../../escaped.wav", "data") });

        CHECK (result.status == ExtractionStatus::rejected);
        CHECK_FALSE (std::filesystem::exists (temp.path / "escaped.wav"));
        CHECK_FALSE (std::filesystem::exists (temp.path.parent_path() / "escaped.wav"));
    }

    SECTION ("symlink entries never materialise")
    {
        sfzfixture::TempDirectory temp;
        const auto result = extract (temp, {
            sfzfixture::file ("pack/keep.wav", "data"),
            sfzfixture::symlink ("pack/escape", "/etc/passwd") });

        CHECK (result.status == ExtractionStatus::rejected);
        CHECK (result.error.find ("not a file or directory") != std::string::npos);
        CHECK_FALSE (std::filesystem::exists (
            std::filesystem::symlink_status (temp.path / "expand/pack/escape")));
    }

    SECTION ("case-colliding names")
    {
        sfzfixture::TempDirectory temp;
        const auto result = extract (temp, {
            sfzfixture::file ("pack/Kick.wav", "one"),
            sfzfixture::file ("pack/kick.WAV", "two") });

        CHECK (result.status == ExtractionStatus::rejected);
        CHECK (result.error.find ("duplicate or case-colliding") != std::string::npos);
    }

    SECTION ("expansion past the catalog budget")
    {
        sfzfixture::TempDirectory temp;
        auto limits = defaultLimits();
        limits.maximumExpandedBytes = 32;
        const auto result = extract (temp, {
            sfzfixture::file ("pack/bomb.wav", std::string (4096, 'x')) }, limits);

        CHECK (result.status == ExtractionStatus::rejected);
        CHECK (result.error.find ("more bytes than the catalog allows") != std::string::npos);
    }

    SECTION ("an archive that is not a ZIP")
    {
        sfzfixture::TempDirectory temp;
        const auto archive = temp.path / "pack.zip";
        sfzfixture::writeBinaryFile (archive, "this is not an archive");
        const auto destination = temp.path / "expand";
        std::filesystem::create_directories (destination);

        ExtractionRequest request;
        request.archiveFile = archive;
        request.destinationDirectory = destination;
        request.limits = defaultLimits();
        CHECK (extractZipArchive (request, {}).status == ExtractionStatus::readFailed);
    }
}

TEST_CASE ("SFZ extraction stops when cancelled", "[sfz][archive]")
{
    sfzfixture::TempDirectory temp;
    const auto archive = temp.path / "pack.zip";
    sfzfixture::writeBinaryFile (archive, sfzfixture::buildZip ({
        sfzfixture::file ("pack/one.wav", std::string (512, 'a')),
        sfzfixture::file ("pack/two.wav", std::string (512, 'b')) }));

    const auto destination = temp.path / "expand";
    std::filesystem::create_directories (destination);

    ExtractionRequest request;
    request.archiveFile = archive;
    request.destinationDirectory = destination;
    request.limits = defaultLimits();

    bool cancelled = false;
    duskstudio::sfz::ExtractionCallbacks callbacks;
    callbacks.isCancelled = [&cancelled] { return cancelled; };
    callbacks.onProgress = [&cancelled] (std::uint64_t) { cancelled = true; };

    CHECK (extractZipArchive (request, callbacks).status == ExtractionStatus::cancelled);
    CHECK_FALSE (std::filesystem::exists (destination / "pack/two.wav"));
}

TEST_CASE ("SFZ extraction requires an empty destination", "[sfz][archive]")
{
    sfzfixture::TempDirectory temp;
    const auto archive = temp.path / "pack.zip";
    sfzfixture::writeBinaryFile (archive,
                                 sfzfixture::buildZip ({ sfzfixture::file ("pack/a.wav", "x") }));

    const auto destination = temp.path / "expand";
    sfzfixture::writeBinaryFile (destination / "stale.wav", "old");

    ExtractionRequest request;
    request.archiveFile = archive;
    request.destinationDirectory = destination;
    request.limits = defaultLimits();
    CHECK (extractZipArchive (request, {}).status == ExtractionStatus::storageFailed);
}

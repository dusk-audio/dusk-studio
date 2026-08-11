#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzDownload.h"
#include "sfz_pack_fixture.h"

#include <string>

namespace
{
using duskstudio::sfz::DownloadRequest;
using duskstudio::sfz::DownloadStatus;
using duskstudio::sfz::downloadArchive;
using duskstudio::sfz::hashFileSha256;

struct Fixture
{
    explicit Fixture (std::string bytes)
    {
        transport.body = std::move (bytes);

        const auto reference = temp.path / "reference.bin";
        sfzfixture::writeBinaryFile (reference, transport.body);

        request.url = "https://catalog.example/pack.zip";
        request.partFile = temp.path / "downloads/pack.zip.part";
        request.destination = temp.path / "downloads/pack.zip";
        request.expectedSha256 = hashFileSha256 (reference);
        request.expectedBytes = transport.body.size();
    }

    duskstudio::sfz::DownloadResult run (const duskstudio::sfz::DownloadCallbacks& callbacks = {})
    {
        return downloadArchive (transport, request, callbacks);
    }

    sfzfixture::TempDirectory temp;
    sfzfixture::FakeTransport transport;
    DownloadRequest request;
};

std::string payload (std::size_t bytes = 256)
{
    std::string out;
    out.reserve (bytes);
    for (std::size_t i = 0; i < bytes; ++i)
        out.push_back (static_cast<char> ('a' + (i % 26)));
    return out;
}
} // namespace

TEST_CASE ("SFZ download verifies and publishes the archive", "[sfz][download]")
{
    Fixture fixture (payload());
    const auto result = fixture.run();

    REQUIRE (result.status == DownloadStatus::completed);
    CHECK (result.downloadedBytes == fixture.request.expectedBytes);
    CHECK (std::filesystem::exists (fixture.request.destination));
    CHECK_FALSE (std::filesystem::exists (fixture.request.partFile));
    CHECK (sfzfixture::readBinaryFile (fixture.request.destination) == fixture.transport.body);
}

TEST_CASE ("SFZ download refuses an archive that fails its digest", "[sfz][download]")
{
    Fixture fixture (payload());
    fixture.request.expectedSha256 = std::string (64, 'b');

    const auto result = fixture.run();

    CHECK (result.status == DownloadStatus::hashMismatch);
    CHECK_FALSE (std::filesystem::exists (fixture.request.destination));
    // A poisoned prefix must never survive to be resumed into a valid archive.
    CHECK_FALSE (std::filesystem::exists (fixture.request.partFile));
}

TEST_CASE ("SFZ download refuses a malformed digest before contacting the server",
           "[sfz][download]")
{
    Fixture fixture (payload());
    fixture.request.expectedSha256 = "not-a-digest";

    CHECK (fixture.run().status == DownloadStatus::transferFailed);
    CHECK (fixture.transport.attempts == 0);
}

TEST_CASE ("SFZ download keeps the partial file when cancelled", "[sfz][download]")
{
    Fixture fixture (payload());
    fixture.transport.chunkBytes = 64;

    bool cancelled = false;
    duskstudio::sfz::DownloadCallbacks callbacks;
    callbacks.isCancelled = [&cancelled] { return cancelled; };
    fixture.transport.onChunkDelivered = [&cancelled] (std::uint64_t) { cancelled = true; };

    const auto result = fixture.run (callbacks);
    REQUIRE (result.status == DownloadStatus::cancelled);
    REQUIRE (std::filesystem::exists (fixture.request.partFile));
    CHECK (std::filesystem::file_size (fixture.request.partFile) == 64);

    cancelled = false;
    fixture.transport.onChunkDelivered = nullptr;
    const auto resumed = fixture.run (callbacks);

    CHECK (resumed.status == DownloadStatus::completed);
    CHECK (fixture.transport.requestedOffsets.back() == 64);
    CHECK (resumed.downloadedBytes == fixture.request.expectedBytes - 64);
    CHECK (sfzfixture::readBinaryFile (fixture.request.destination) == fixture.transport.body);
}

TEST_CASE ("SFZ download resumes a dropped connection within its attempt budget",
           "[sfz][download]")
{
    Fixture fixture (payload());
    fixture.transport.chunkBytes = 64;
    fixture.transport.failAfterChunks = 1;
    fixture.request.maximumAttempts = 5;

    const auto result = fixture.run();

    CHECK (result.status == DownloadStatus::completed);
    CHECK (fixture.transport.attempts == 4);
    CHECK (fixture.transport.requestedOffsets == std::vector<std::uint64_t> { 0, 64, 128, 192 });
}

TEST_CASE ("SFZ download gives up once the attempt budget is spent", "[sfz][download]")
{
    Fixture fixture (payload());
    fixture.transport.chunkBytes = 64;
    fixture.transport.failAfterChunks = 1;
    fixture.request.maximumAttempts = 2;

    const auto result = fixture.run();

    CHECK (result.status == DownloadStatus::transferFailed);
    CHECK (fixture.transport.attempts == 2);
    CHECK_FALSE (std::filesystem::exists (fixture.request.destination));
    CHECK (std::filesystem::file_size (fixture.request.partFile) == 128);
}

TEST_CASE ("SFZ download restarts when the server ignores the byte range", "[sfz][download]")
{
    Fixture fixture (payload());
    fixture.transport.chunkBytes = 64;
    fixture.transport.honourRange = false;
    sfzfixture::writeBinaryFile (fixture.request.partFile, std::string (64, 'z'));

    const auto result = fixture.run();

    CHECK (result.status == DownloadStatus::completed);
    CHECK (fixture.transport.requestedOffsets.front() == 64);
    CHECK (sfzfixture::readBinaryFile (fixture.request.destination) == fixture.transport.body);
}

TEST_CASE ("SFZ download discards a partial file longer than the catalog promises",
           "[sfz][download]")
{
    Fixture fixture (payload());
    sfzfixture::writeBinaryFile (fixture.request.partFile,
                                 std::string (fixture.request.expectedBytes + 32, 'z'));

    const auto result = fixture.run();

    CHECK (result.status == DownloadStatus::completed);
    CHECK (fixture.transport.requestedOffsets.front() == 0);
    CHECK (sfzfixture::readBinaryFile (fixture.request.destination) == fixture.transport.body);
}

TEST_CASE ("SFZ download reports a short archive", "[sfz][download]")
{
    Fixture fixture (payload());
    fixture.request.expectedBytes += 64;
    fixture.request.maximumAttempts = 2;

    const auto result = fixture.run();

    CHECK (result.status == DownloadStatus::sizeMismatch);
    CHECK_FALSE (std::filesystem::exists (fixture.request.destination));
}

TEST_CASE ("SFZ download reuses a complete partial file", "[sfz][download]")
{
    Fixture fixture (payload());
    sfzfixture::writeBinaryFile (fixture.request.partFile, fixture.transport.body);

    const auto result = fixture.run();

    CHECK (result.status == DownloadStatus::completed);
    CHECK (fixture.transport.attempts == 0);
    CHECK (std::filesystem::exists (fixture.request.destination));
}

#pragma once

#include "engine/sfz/SfzDownload.h"
#include "engine/sfz/SfzInstall.h"
#include "engine/sfz/SfzTransport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// Deterministic fixtures for the SFZ transfer and install tests: a store-method
// ZIP writer that can emit entries no honest archiver would (traversal names,
// links, case collisions) and an in-memory transport so no test touches the
// network.
namespace sfzfixture
{
namespace stdfs = std::filesystem;

enum class ZipEntryKind
{
    file,
    directory,
    symlink
};

struct ZipEntry
{
    std::string name;
    ZipEntryKind kind { ZipEntryKind::file };
    std::string content;
};

inline ZipEntry file (std::string name, std::string content)
{
    return { std::move (name), ZipEntryKind::file, std::move (content) };
}

inline ZipEntry directory (std::string name)
{
    return { std::move (name), ZipEntryKind::directory, {} };
}

inline ZipEntry symlink (std::string name, std::string target)
{
    return { std::move (name), ZipEntryKind::symlink, std::move (target) };
}

inline std::uint32_t crc32 (const std::string& data)
{
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : data)
    {
        crc ^= static_cast<unsigned char> (byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xffffffffu;
}

inline void appendLittleEndian (std::string& out, std::uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i)
        out.push_back (static_cast<char> ((value >> (8 * i)) & 0xff));
}

// Minimal store-method ZIP. "version made by" claims UNIX so the external
// attributes carry a mode, which is how a ZIP describes a symlink.
inline std::string buildZip (const std::vector<ZipEntry>& entries)
{
    std::string local;
    std::string central;
    std::uint32_t entryCount = 0;

    for (const auto& entry : entries)
    {
        const auto isDirectory = entry.kind == ZipEntryKind::directory;
        auto name = entry.name;
        if (isDirectory && (name.empty() || name.back() != '/'))
            name.push_back ('/');

        const auto& payload = isDirectory ? std::string() : entry.content;
        const auto offset = static_cast<std::uint32_t> (local.size());
        const auto checksum = crc32 (payload);
        const auto size = static_cast<std::uint32_t> (payload.size());

        appendLittleEndian (local, 0x04034b50u, 4);
        appendLittleEndian (local, 20, 2);
        appendLittleEndian (local, 0, 2);
        appendLittleEndian (local, 0, 2);
        appendLittleEndian (local, 0, 2);
        appendLittleEndian (local, 0, 2);
        appendLittleEndian (local, checksum, 4);
        appendLittleEndian (local, size, 4);
        appendLittleEndian (local, size, 4);
        appendLittleEndian (local, static_cast<std::uint32_t> (name.size()), 2);
        appendLittleEndian (local, 0, 2);
        local += name;
        local += payload;

        std::uint32_t externalAttributes = 0;
        switch (entry.kind)
        {
            case ZipEntryKind::directory:
                externalAttributes = (0040755u << 16) | 0x10u;
                break;
            case ZipEntryKind::symlink:
                externalAttributes = 0120777u << 16;
                break;
            case ZipEntryKind::file:
                externalAttributes = 0100644u << 16;
                break;
        }

        appendLittleEndian (central, 0x02014b50u, 4);
        appendLittleEndian (central, (3u << 8) | 20u, 2);
        appendLittleEndian (central, 20, 2);
        appendLittleEndian (central, 0, 2);
        appendLittleEndian (central, 0, 2);
        appendLittleEndian (central, 0, 2);
        appendLittleEndian (central, 0, 2);
        appendLittleEndian (central, checksum, 4);
        appendLittleEndian (central, size, 4);
        appendLittleEndian (central, size, 4);
        appendLittleEndian (central, static_cast<std::uint32_t> (name.size()), 2);
        appendLittleEndian (central, 0, 2);
        appendLittleEndian (central, 0, 2);
        appendLittleEndian (central, 0, 2);
        appendLittleEndian (central, 0, 2);
        appendLittleEndian (central, externalAttributes, 4);
        appendLittleEndian (central, offset, 4);
        central += name;
        ++entryCount;
    }

    std::string archive = local;
    const auto centralOffset = static_cast<std::uint32_t> (archive.size());
    archive += central;
    appendLittleEndian (archive, 0x06054b50u, 4);
    appendLittleEndian (archive, 0, 2);
    appendLittleEndian (archive, 0, 2);
    appendLittleEndian (archive, entryCount, 2);
    appendLittleEndian (archive, entryCount, 2);
    appendLittleEndian (archive, static_cast<std::uint32_t> (central.size()), 4);
    appendLittleEndian (archive, centralOffset, 4);
    appendLittleEndian (archive, 0, 2);
    return archive;
}

inline void writeBinaryFile (const stdfs::path& path, const std::string& bytes)
{
    stdfs::create_directories (path.parent_path());
    std::ofstream out (path, std::ios::binary | std::ios::trunc);
    out.write (bytes.data(), static_cast<std::streamsize> (bytes.size()));
}

inline std::string readBinaryFile (const stdfs::path& path)
{
    std::ifstream in (path, std::ios::binary);
    return std::string (std::istreambuf_iterator<char> (in),
                        std::istreambuf_iterator<char>());
}

struct TempDirectory
{
    TempDirectory()
    {
        static std::atomic<unsigned> counter { 0 };
        std::random_device entropy;
        for (int attempt = 0; attempt < 128; ++attempt)
        {
            auto candidate = stdfs::temp_directory_path()
                / ("dusk-sfz-test-" + std::to_string (counter.fetch_add (1)) + "-"
                   + std::to_string (entropy()) + "-"
                   + std::to_string (
                       static_cast<unsigned long long> (
                           std::chrono::steady_clock::now().time_since_epoch().count())));
            std::error_code ec;
            // create_directory returns false with no error when the name is
            // already taken, so a true return is proof this process created the
            // directory - no name-then-create window a parallel ctest process
            // could race through.
            if (stdfs::create_directory (candidate, ec) && ! ec)
            {
                path = std::move (candidate);
                return;
            }
        }
        throw std::runtime_error ("could not create a unique temp directory");
    }

    ~TempDirectory()
    {
        std::error_code ec;
        stdfs::remove_all (path, ec);
    }

    TempDirectory (const TempDirectory&) = delete;
    TempDirectory& operator= (const TempDirectory&) = delete;

    stdfs::path path;
};

// Serves a byte string in fixed-size chunks. Every deviation a real server can
// throw at the downloader - ignoring a range request, dying mid-stream, sending
// more than promised - is a field on this struct.
struct FakeTransport final : duskstudio::sfz::Transport
{
    std::string body;
    bool honourRange { true };
    std::size_t chunkBytes { 64 };
    int failAfterChunks { -1 };
    unsigned attempts { 0 };
    std::vector<std::uint64_t> requestedOffsets;
    std::function<void (std::uint64_t deliveredBytes)> onChunkDelivered;

    duskstudio::sfz::TransferResult fetch (
        const duskstudio::sfz::TransferRequest& request,
        const duskstudio::sfz::TransferCallbacks& callbacks) override
    {
        using namespace duskstudio::sfz;

        ++attempts;
        requestedOffsets.push_back (request.resumeOffset);

        TransferResult result;
        if (request.resumeOffset > body.size())
        {
            result.error = "requested range not satisfiable";
            return result;
        }

        const auto resumeAccepted = request.resumeOffset > 0 && honourRange;
        std::size_t position = resumeAccepted ? static_cast<std::size_t> (request.resumeOffset) : 0;

        if (callbacks.onResponse
            && ! callbacks.onResponse (resumeAccepted, body.size()))
        {
            result.status = TransferStatus::cancelled;
            return result;
        }

        int chunks = 0;
        while (position < body.size())
        {
            if (failAfterChunks >= 0 && chunks >= failAfterChunks)
            {
                result.error = "the connection dropped";
                return result;
            }

            const auto bytes = std::min (chunkBytes, body.size() - position);
            if (callbacks.onData
                && ! callbacks.onData (
                    reinterpret_cast<const unsigned char*> (body.data() + position), bytes))
            {
                result.status = TransferStatus::cancelled;
                return result;
            }

            position += bytes;
            result.receivedBytes += bytes;
            ++chunks;

            if (onChunkDelivered)
                onChunkDelivered (result.receivedBytes);
            if (callbacks.onProgress && ! callbacks.onProgress (position, body.size()))
            {
                result.status = TransferStatus::cancelled;
                return result;
            }
        }

        result.status = TransferStatus::completed;
        return result;
    }
};

inline const std::string kLicenseText = "CC0 1.0 Universal\n";
inline const std::string kInstrumentText = "<region> sample=Samples/kick.wav\n";

// A complete, installable pack: the archive the fake transport serves and the
// catalog metadata that describes it stay in step, so a test only has to say
// what it changed.
struct PackFixture
{
    PackFixture()
    {
        layout.root = temp.path / "library";
        entries = { directory ("pack"),
                    file ("pack/LICENSE", kLicenseText),
                    file ("pack/Kit.sfz", kInstrumentText),
                    file ("pack/Samples/kick.wav", std::string (128, 'k')) };
    }

    duskstudio::sfz::CatalogPack pack()
    {
        transport.body = buildZip (entries);
        const auto archive = temp.path / "archive.zip";
        writeBinaryFile (archive, transport.body);

        const auto licensePath = temp.path / "license.txt";
        writeBinaryFile (licensePath, kLicenseText);

        duskstudio::sfz::CatalogPack described;
        described.id = "pack";
        described.releaseId = "v1.0.0";
        described.displayName = "Pack";
        described.author = "Dusk";
        described.sourceUrl = "https://example.test/source";
        described.downloadUrl = "https://example.test/pack.zip";
        described.archiveSha256 = duskstudio::sfz::hashFileSha256 (archive);
        described.archiveFormat = "zip";
        described.expectedRoot = "pack";
        described.compressedBytes = transport.body.size();
        described.expandedBytes = 1024 * 1024;
        described.maxFiles = 64;
        described.license.spdx = "CC0-1.0";
        described.license.file = "LICENSE";
        described.license.fileSha256 = duskstudio::sfz::hashFileSha256 (licensePath);
        described.license.redistributionAllowed = true;
        described.license.mirrorAllowed = true;
        described.instruments.push_back (
            duskstudio::sfz::CatalogInstrument { "kit", "Kit", "Kit.sfz" });
        return described;
    }

    duskstudio::sfz::InstallResult install (
        const duskstudio::sfz::InstallCallbacks& callbacks = {})
    {
        return duskstudio::sfz::installPack (transport, pack(), layout, {}, callbacks);
    }

    bool stagingIsClean() const
    {
        std::error_code ec;
        return ! stdfs::exists (layout.stagingDirectory(), ec)
            || stdfs::is_empty (layout.stagingDirectory(), ec);
    }

    TempDirectory temp;
    FakeTransport transport;
    duskstudio::sfz::StoreLayout layout;
    std::vector<ZipEntry> entries;
};
} // namespace sfzfixture

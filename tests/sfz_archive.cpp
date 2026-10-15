#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzArchive.h"
#include "foundation/Fs.h"

#include <sodium.h>

#include <array>
#include <cstdint>
#include <clocale>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if ! defined(_WIN32)
 #include <locale.h>
 #if defined(__APPLE__)
  #include <xlocale.h>
 #endif
#endif

using namespace duskstudio::sfz;

namespace
{
struct ScratchTree
{
    ScratchTree() : root (dusk::fs::createUniqueTempDirectory ("dusk-sfz-archive-")) {}
    ~ScratchTree()
    {
        std::error_code error;
        std::filesystem::remove_all (root, error);
    }
    std::filesystem::path root;
};

struct Member
{
    std::string name;
    std::string data;
    bool directory = false;
    std::string symlink {};
};

std::vector<unsigned char> zip (const std::vector<Member>& members)
{
    std::vector<unsigned char> bytes, central;
    const auto append = [] (auto& target, std::uint32_t value, unsigned width)
    {
        for (unsigned i = 0; i < width; ++i)
            target.push_back (static_cast<unsigned char> (value >> (8 * i)));
    };
    const auto checksum = [] (const std::string& data)
    {
        std::uint32_t crc = 0xffffffff;
        for (const char byte : data)
        {
            crc ^= static_cast<unsigned char> (byte);
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
        return ~crc;
    };
    // Write raw UTF-8 stored entries so fixture creation cannot depend on the process locale.
    for (const auto& member : members)
    {
        REQUIRE (member.name.size() <= 0xffff);
        REQUIRE (member.data.size() < 2 * 1024 * 1024);
        const auto data = ! member.symlink.empty() ? member.symlink : member.directory ? std::string {} : member.data;
        const auto crc = checksum (data);
        const auto size = static_cast<std::uint32_t> (data.size());
        const auto offset = static_cast<std::uint32_t> (bytes.size());
        const auto nameSize = static_cast<std::uint32_t> (member.name.size());
        const std::uint32_t mode = ! member.symlink.empty() ? 0120644U : member.directory ? 0040755U : 0100644U;
        append (bytes, 0x04034b50, 4); append (bytes, 20, 2); append (bytes, 0x0800, 2);
        append (bytes, 0, 2); append (bytes, 0, 2); append (bytes, 33, 2); append (bytes, crc, 4);
        append (bytes, size, 4); append (bytes, size, 4); append (bytes, nameSize, 2); append (bytes, 0, 2);
        bytes.insert (bytes.end(), member.name.begin(), member.name.end());
        bytes.insert (bytes.end(), data.begin(), data.end());
        append (central, 0x02014b50, 4); append (central, 0x0314, 2); append (central, 20, 2);
        append (central, 0x0800, 2); append (central, 0, 2); append (central, 0, 2); append (central, 33, 2);
        append (central, crc, 4); append (central, size, 4); append (central, size, 4); append (central, nameSize, 2);
        append (central, 0, 2); append (central, 0, 2); append (central, 0, 2); append (central, 0, 2);
        append (central, (mode << 16) | (member.directory ? 0x10U : 0U), 4); append (central, offset, 4);
        central.insert (central.end(), member.name.begin(), member.name.end());
    }
    REQUIRE (members.size() <= 0xffff);
    const auto offset = static_cast<std::uint32_t> (bytes.size());
    bytes.insert (bytes.end(), central.begin(), central.end());
    append (bytes, 0x06054b50, 4); append (bytes, 0, 2); append (bytes, 0, 2);
    append (bytes, static_cast<std::uint32_t> (members.size()), 2); append (bytes, static_cast<std::uint32_t> (members.size()), 2);
    append (bytes, static_cast<std::uint32_t> (central.size()), 4); append (bytes, offset, 4); append (bytes, 0, 2);
    return bytes;
}

void writeBytes (const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
    std::ofstream stream (path, std::ios::binary);
    stream.write (reinterpret_cast<const char*> (bytes.data()),
                  static_cast<std::streamsize> (bytes.size()));
    REQUIRE (stream.good());
}

ArchiveRequest requestFor (ScratchTree& tree, const std::vector<unsigned char>& bytes)
{
    ArchiveRequest request;
    request.archive = tree.root / "download.zip";
    request.stagingDirectory = tree.root / "staging";
    writeBytes (request.archive, bytes);
    return request;
}

std::string readFile (const std::filesystem::path& path)
{
    std::ifstream stream (path, std::ios::binary);
    return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char>() };
}

std::string sha256 (const std::string& source)
{
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest {};
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
    REQUIRE (crypto_hash_sha256 (digest.data(),
                                reinterpret_cast<const unsigned char*> (source.data()),
                                source.size()) == 0);
    REQUIRE (sodium_bin2hex (hex.data(), hex.size(), digest.data(), digest.size()) != nullptr);
    return hex.data();
}

std::uint32_t get32 (const std::vector<unsigned char>& bytes, std::size_t at)
{
    REQUIRE (at + 4 <= bytes.size());
    return std::uint32_t (bytes[at]) | (std::uint32_t (bytes[at + 1]) << 8)
        | (std::uint32_t (bytes[at + 2]) << 16) | (std::uint32_t (bytes[at + 3]) << 24);
}

void put (std::vector<unsigned char>& bytes, std::size_t at, std::uint64_t value, std::size_t size)
{
    REQUIRE (at + size <= bytes.size());
    for (std::size_t i = 0; i < size; ++i)
        bytes[at + i] = static_cast<unsigned char> ((value >> (i * 8)) & 0xff);
}

std::size_t signature (const std::vector<unsigned char>& bytes, std::uint32_t value)
{
    for (std::size_t at = 0; at + 4 <= bytes.size(); ++at)
        if (get32 (bytes, at) == value) return at;
    FAIL ("Expected ZIP signature is missing");
    return 0;
}
} // namespace

TEST_CASE ("SFZ portable paths reject escape and Windows aliases")
{
    std::string error;
    for (const std::string path : { "", "/piano.sfz", "\\\\server\\piano.sfz", "C:/piano.sfz",
                                   "../piano.sfz", "a/../piano.sfz", "./piano.sfz", "a//piano.sfz",
                                   "a\\piano.sfz", "a/piano.sfz.", "a /piano.sfz", "CON.sfz",
                                   "a/Lpt1.wav", "COM¹.wav", "piano:stream.sfz", "a/piano?.sfz" })
    {
        CAPTURE (path);
        CHECK_FALSE (validateRelativePath (path, error));
        CHECK_FALSE (error.empty());
    }
    CHECK_FALSE (validateRelativePath (std::string ("a\0b.sfz", 7), error));
    CHECK_FALSE (validateRelativePath (std::string ("a\x1f" "b.sfz", 7), error));
    CHECK_FALSE (validateRelativePath (std::string ("a\xc0\xaf" "b.sfz", 8), error));
    CHECK (validateRelativePath (u8"Étude/ピアノ.sfz", error));
    CHECK (error.empty());
}

TEST_CASE ("SFZ portable path keys detect Unicode normalization and case aliases")
{
    CHECK (portablePathKey ("Samples/Piano.sfz") == portablePathKey ("samples/piano.SFZ"));
    CHECK (portablePathKey (u8"ÉTUDES.sfz") == portablePathKey (u8"e\u0301tudes.sfz"));
    CHECK (portablePathKey (u8"Straße.sfz") == portablePathKey ("STRASSE.sfz"));
    CHECK (portablePathKey (u8"ı.sfz") == portablePathKey ("I.sfz"));
    CHECK (portablePathKey (u8"a\u200bb.sfz") == portablePathKey ("ab.sfz"));
    std::string error;
    CHECK_FALSE (validateRelativePath (u8".\u200b.", error));
    CHECK_FALSE (validateRelativePath (u8"C\u200bON.sfz", error));
    CHECK (isSha256 (std::string (64, 'a')));
    CHECK_FALSE (isSha256 (std::string (64, 'A')));
}

TEST_CASE ("SFZ ZIP extraction strips only the expected root and verifies its license")
{
    ScratchTree tree;
    const std::string instrument = "<region> sample=Samples/tone.wav";
    const std::string license = "CC0-1.0";
    std::string entrypoint;
    SECTION ("Composed Unicode pathname") { entrypoint = u8"Étude.sfz"; }
    SECTION ("Decomposed Unicode pathname") { entrypoint = u8"E\u0301tude.sfz"; }
    auto request = requestFor (tree, zip ({
        { "pack-release/", {}, true }, { "pack-release/" + entrypoint, instrument },
        { "pack-release/Samples/tone.wav", "sample bytes" }, { "pack-release/LICENSE", license }
    }));
    request.expectedRoot = "pack-release";
    request.licensePath = "LICENSE";
    request.licenseSha256 = sha256 (license);
    request.expectedEntrypoints = { entrypoint };
    OperationProgress latest;
    OperationControl control;
    control.progress = [&latest] (const OperationProgress& progress) { latest = progress; };
    const std::string originalCtype = std::setlocale (LC_CTYPE, nullptr);
   #if defined(_WIN32)
    const int originalLocaleMode = _configthreadlocale (0);
   #else
    const auto originalThreadLocale = uselocale (nullptr);
   #endif
    const auto result = extractZip (request, control);
    INFO (result.error);
    REQUIRE (result);
    CHECK (std::string (std::setlocale (LC_CTYPE, nullptr)) == originalCtype);
   #if defined(_WIN32)
    CHECK (_configthreadlocale (0) == originalLocaleMode);
   #else
    CHECK (uselocale (nullptr) == originalThreadLocale);
   #endif
    REQUIRE (result.sfzPaths == std::vector<std::string> { entrypoint });
    CHECK (result.files == 3);
    CHECK (result.expandedBytes == instrument.size() + license.size() + 12);
    CHECK (readFile (request.stagingDirectory / std::filesystem::u8path (entrypoint)) == instrument);
    CHECK (readFile (request.stagingDirectory / "LICENSE") == license);
    CHECK_FALSE (std::filesystem::exists (request.stagingDirectory / "pack-release"));
    CHECK (latest.completedFiles == result.files);
    CHECK (latest.completedBytes == latest.totalBytes);
}

TEST_CASE ("SFZ ZIP extraction keeps archive paths when no root is requested")
{
    ScratchTree tree;
    const auto request = requestFor (tree, zip ({ { "pack/piano.sfz", "instrument" } }));
    const auto result = extractZip (request);
    INFO (result.error);
    REQUIRE (result);
    CHECK (result.sfzPaths == std::vector<std::string> { "pack/piano.sfz" });
    CHECK (readFile (request.stagingDirectory / "pack/piano.sfz") == "instrument");
}

TEST_CASE ("SFZ ZIP extraction rejects entries outside the exact root")
{
    for (const std::string path : { "pack-other/piano.sfz", "other/piano.sfz", "pack" })
    {
        ScratchTree tree;
        auto request = requestFor (tree, zip ({ { path, "instrument" } }));
        request.expectedRoot = "pack";
        const auto result = extractZip (request);
        CAPTURE (path, result.error);
        CHECK (result.status == OperationStatus::Failed);
        CHECK_FALSE (std::filesystem::exists (request.stagingDirectory));
    }
}

TEST_CASE ("SFZ ZIP extraction rejects unsafe archive pathnames before writing")
{
    for (const std::string path : { "../escape.sfz", "/escape.sfz", "C:/escape.sfz",
                                   "samples\\escape.sfz", "NUL.sfz", "folder./piano.sfz" })
    {
        ScratchTree tree;
        const auto request = requestFor (tree, zip ({ { path, "instrument" } }));
        const auto result = extractZip (request);
        CAPTURE (path, result.error);
        CHECK (result.status == OperationStatus::Failed);
        CHECK_FALSE (std::filesystem::exists (request.stagingDirectory));
        CHECK_FALSE (std::filesystem::exists (tree.root / "escape.sfz"));
    }
}

TEST_CASE ("SFZ ZIP extraction rejects embedded NUL in the raw ZIP pathname")
{
    ScratchTree tree;
    auto bytes = zip ({ { "piano.sfz", "instrument" } });
    const auto local = signature (bytes, 0x04034b50);
    const auto central = signature (bytes, 0x02014b50);
    bytes[local + 30 + 2] = 0;
    bytes[central + 46 + 2] = 0;
    const auto request = requestFor (tree, bytes);
    CHECK (extractZip (request).status == OperationStatus::Failed);
    CHECK_FALSE (std::filesystem::exists (request.stagingDirectory));
}

TEST_CASE ("SFZ ZIP extraction rejects duplicate and filesystem-colliding names")
{
    const std::vector<std::vector<Member>> cases {
        { { "piano.sfz", "one" }, { "piano.sfz", "two" } },
        { { "piano.sfz", "one" }, { "PIANO.SFZ", "two" } },
        { { "Samples/one.sfz", "one" }, { "samples/two.sfz", "two" } },
        { { u8"É.sfz", "one" }, { u8"e\u0301.sfz", "two" } },
        { { "samples", "one" }, { "samples/piano.sfz", "two" } },
        { { "samples/", {}, true }, { "samples", "one" }, { "piano.sfz", "two" } }
    };
    for (const auto& members : cases)
    {
        ScratchTree tree;
        const auto request = requestFor (tree, zip (members));
        const auto result = extractZip (request);
        INFO (result.error);
        CHECK (result.status == OperationStatus::Failed);
        CHECK_FALSE (std::filesystem::exists (request.stagingDirectory));
    }
}

TEST_CASE ("SFZ ZIP extraction rejects symlinks and special entry metadata")
{
    SECTION ("symlink")
    {
        ScratchTree tree;
        const auto request = requestFor (tree, zip ({
            { "piano.sfz", "instrument" }, { "samples", {}, false, "../outside" }
        }));
        CHECK (extractZip (request).status == OperationStatus::Failed);
        CHECK_FALSE (std::filesystem::exists (request.stagingDirectory));
    }
    for (const std::uint32_t attributes : { 0x600001a4U, 0x800005a4U })
    {
        ScratchTree tree;
        auto bytes = zip ({ { "piano.sfz", "instrument" } });
        const auto central = signature (bytes, 0x02014b50);
        put (bytes, central + 38, attributes, 4);
        const auto request = requestFor (tree, bytes);
        CHECK (extractZip (request).status == OperationStatus::Failed);
        CHECK_FALSE (std::filesystem::exists (request.stagingDirectory));
    }
}

TEST_CASE ("SFZ ZIP extraction refuses encrypted and conflicting local headers")
{
    ScratchTree tree;
    auto bytes = zip ({ { "piano.sfz", "instrument" } });
    const auto local = signature (bytes, 0x04034b50);
    const auto central = signature (bytes, 0x02014b50);
    SECTION ("encrypted")
    {
        bytes[local + 6] |= 1;
        bytes[central + 8] |= 1;
    }
    SECTION ("local and central names differ")
    {
        bytes[local + 30] = 'x';
    }
    SECTION ("canonically equivalent local and central names still differ")
    {
        bytes = zip ({ { u8"e\u0301\u0323.sfz", "instrument" } });
        std::swap (bytes[32], bytes[34]);
    }
    const auto request = requestFor (tree, bytes);
    CHECK (extractZip (request).status == OperationStatus::Failed);
    CHECK_FALSE (std::filesystem::exists (request.stagingDirectory));
}

TEST_CASE ("SFZ ZIP extraction enforces declared byte file and path limits")
{
    ScratchTree tree;
    auto request = requestFor (tree, zip ({
        { "folder/piano.sfz", "0123456789" }, { "folder/sample.wav", "0123456789" }
    }));
    SECTION ("archive bytes") { request.limits.maxArchiveBytes = 1; }
    SECTION ("expanded bytes") { request.limits.maxExpandedBytes = 19; }
    SECTION ("individual file bytes") { request.limits.maxFileBytes = 9; }
    SECTION ("file count") { request.limits.maxFiles = 1; }
    SECTION ("implicit directories") { request.limits.maxFiles = 2; }
    SECTION ("path depth") { request.limits.paths.maxDepth = 1; }
    SECTION ("path bytes") { request.limits.paths.maxBytes = 10; }
    const auto result = extractZip (request);
    INFO (result.error);
    CHECK (result.status == OperationStatus::Failed);
    CHECK_FALSE (std::filesystem::exists (request.stagingDirectory));
}

TEST_CASE ("SFZ ZIP extraction enforces actual bytes when ZIP declared sizes lie")
{
    ScratchTree tree;
    auto bytes = zip ({ { "piano.sfz", "0123456789" } });
    const auto central = signature (bytes, 0x02014b50);
    put (bytes, central + 24, 1, 4);
    auto request = requestFor (tree, bytes);
    request.limits.maxExpandedBytes = 1;
    const auto result = extractZip (request);
    CHECK (result.status == OperationStatus::Failed);
    CHECK (result.expandedBytes <= 1);
}

TEST_CASE ("SFZ ZIP extraction rejects corrupt member data")
{
    ScratchTree tree;
    auto bytes = zip ({ { "piano.sfz", "0123456789" } });
    const auto local = signature (bytes, 0x04034b50);
    const auto names = std::size_t (bytes[local + 26]) + (std::size_t (bytes[local + 27]) << 8);
    const auto extras = std::size_t (bytes[local + 28]) + (std::size_t (bytes[local + 29]) << 8);
    bytes[local + 30 + names + extras] ^= 1;
    const auto request = requestFor (tree, bytes);
    CHECK (extractZip (request).status == OperationStatus::Failed);
}

TEST_CASE ("SFZ ZIP extraction refuses fake and truncated ZIP payloads")
{
    ScratchTree tree;
    std::vector<unsigned char> bytes;
    SECTION ("plain text") { bytes.assign ({ 'P', 'K', 'n', 'o', 't', 'a', 'z', 'i', 'p' }); }
    SECTION ("truncated")
    {
        bytes = zip ({ { "piano.sfz", "instrument" } });
        bytes.resize (bytes.size() - 12);
    }
    SECTION ("forged end directory")
    {
        bytes.resize (22);
        put (bytes, 0, 0x06054b50, 4);
        put (bytes, 8, 1, 2);
        put (bytes, 10, 1, 2);
        put (bytes, 12, 0xffffffff, 4);
    }
    const auto request = requestFor (tree, bytes);
    CHECK (extractZip (request).status == OperationStatus::Failed);
    CHECK_FALSE (std::filesystem::exists (request.stagingDirectory));
}

TEST_CASE ("SFZ ZIP extraction requires its expected entrypoint and license")
{
    ScratchTree tree;
    auto request = requestFor (tree, zip ({
        { "piano.sfz", "instrument" }, { "LICENSE", "CC0-1.0" }
    }));
    SECTION ("wrong license hash")
    {
        request.licensePath = "LICENSE";
        request.licenseSha256 = sha256 ("different");
    }
    SECTION ("missing license")
    {
        request.licensePath = "COPYING";
        request.licenseSha256 = sha256 ("CC0-1.0");
    }
    SECTION ("missing entrypoint") { request.expectedEntrypoints = { "missing.sfz" }; }
    const auto result = extractZip (request);
    CHECK (result.status == OperationStatus::Failed);
    CHECK_FALSE (std::filesystem::exists (tree.root / "installed"));
}

TEST_CASE ("SFZ ZIP extraction refuses a populated staging directory")
{
    ScratchTree tree;
    const auto request = requestFor (tree, zip ({ { "piano.sfz", "instrument" } }));
    REQUIRE (std::filesystem::create_directory (request.stagingDirectory));
    std::ofstream (request.stagingDirectory / "keep.txt") << "keep";
    CHECK (extractZip (request).status == OperationStatus::Failed);
    CHECK (readFile (request.stagingDirectory / "keep.txt") == "keep");
    CHECK_FALSE (std::filesystem::exists (request.stagingDirectory / "piano.sfz"));
}

TEST_CASE ("SFZ ZIP extraction refuses symlinked staging and parent directories")
{
    ScratchTree tree;
    const auto request = requestFor (tree, zip ({
        { "piano.sfz", "instrument" }, { "samples/tone.wav", "sample" }
    }));
    const auto outside = tree.root / "outside";
    REQUIRE (std::filesystem::create_directory (outside));
    std::error_code error;
    SECTION ("staging root is a link")
    {
        std::filesystem::create_directory_symlink (outside, request.stagingDirectory, error);
        if (error) SKIP ("symlinks are unavailable");
        CHECK (extractZip (request).status == OperationStatus::Failed);
    }
    SECTION ("parent replaced between extracted entries")
    {
        bool attempted = false;
        OperationControl control;
        control.progress = [&] (const OperationProgress& progress)
        {
            if (progress.completedFiles != 1 || attempted) return;
            attempted = true;
            std::filesystem::create_directory_symlink (
                outside, request.stagingDirectory / "samples", error);
        };
        const auto result = extractZip (request, control);
        REQUIRE (attempted);
        if (error) SKIP ("symlinks are unavailable");
        CHECK (result.status == OperationStatus::Failed);
    }
    CHECK (std::filesystem::is_empty (outside));
}

TEST_CASE ("SFZ ZIP extraction pauses or cancels between bounded data chunks")
{
    ScratchTree tree;
    const auto request = requestFor (tree, zip ({ { "piano.sfz", std::string (256 * 1024, 'x') } }));
    bool interrupted = false;
    OperationControl control;
    control.progress = [&] (const OperationProgress& progress)
        { if (progress.completedBytes >= 64 * 1024) interrupted = true; };
    OperationStatus expected = OperationStatus::Paused;
    SECTION ("pause") { control.paused = [&] { return interrupted; }; }
    SECTION ("cancel")
    {
        expected = OperationStatus::Cancelled;
        control.cancelled = [&] { return interrupted; };
    }
    const auto result = extractZip (request, control);
    CHECK (result.status == expected);
    CHECK (result.files == 0);
    CHECK (result.expandedBytes >= 64 * 1024);
    CHECK (result.expandedBytes < 256 * 1024);
    CHECK (result.sfzPaths.empty());
    CHECK_FALSE (std::filesystem::exists (tree.root / "installed"));
}

TEST_CASE ("SFZ ZIP extraction reads a ZIP64 end directory")
{
    ScratchTree tree;
    auto bytes = zip ({ { "piano.sfz", "instrument" } });
    const auto eocd = signature (bytes, 0x06054b50);
    const auto directoryBytes = get32 (bytes, eocd + 12);
    const auto directoryOffset = get32 (bytes, eocd + 16);
    const auto count = std::uint16_t (bytes[eocd + 10])
        | (std::uint16_t (bytes[eocd + 11]) << 8);
    std::vector<unsigned char> records (76);
    put (records, 0, 0x06064b50, 4);
    put (records, 4, 44, 8);
    put (records, 12, 45, 2);
    put (records, 14, 45, 2);
    put (records, 24, static_cast<std::uint64_t> (count), 8);
    put (records, 32, static_cast<std::uint64_t> (count), 8);
    put (records, 40, directoryBytes, 8);
    put (records, 48, directoryOffset, 8);
    put (records, 56, 0x07064b50, 4);
    put (records, 64, eocd, 8);
    put (records, 72, 1, 4);
    bytes.insert (bytes.begin() + static_cast<std::ptrdiff_t> (eocd), records.begin(), records.end());
    put (bytes, eocd + records.size() + 8, 0xffff, 2);
    put (bytes, eocd + records.size() + 10, 0xffff, 2);
    put (bytes, eocd + records.size() + 12, 0xffffffff, 4);
    put (bytes, eocd + records.size() + 16, 0xffffffff, 4);
    const auto request = requestFor (tree, bytes);
    const auto result = extractZip (request);
    INFO (result.error);
    REQUIRE (result);
    CHECK (readFile (request.stagingDirectory / "piano.sfz") == "instrument");
}

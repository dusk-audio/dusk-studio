#include "SfzPackStore.h"
#include "SfzLibraryLocations.h"
#include "SfzPackIndex.h"
#include "SfzPackIntegrity.h"
#include "../../foundation/Fs.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <system_error>

#if ! defined(_WIN32)
 #include <fcntl.h>
#endif

namespace duskstudio::sfz
{
namespace
{
namespace fs = std::filesystem;
using Json = dusk::json::Json;
constexpr const char* kReceipt = ".dusk-pack.json";

bool syncPath (const fs::path& path, bool directory, std::error_code& error)
{
    error.clear();
#if defined(_WIN32)
    // Directory publication is flushed by MoveFileEx's write-through rename.
    if (directory) return true;
    const auto handle = CreateFileW (dusk::fs::nativeIOPath (path).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    { error = { static_cast<int> (GetLastError()), std::system_category() }; return false; }
    BY_HANDLE_FILE_INFORMATION info {};
    const bool read = GetFileInformationByHandle (handle, &info) != 0;
    if (! read) error = { static_cast<int> (GetLastError()), std::system_category() };
    const bool ordinary = read && (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
    const bool flushed = ordinary && FlushFileBuffers (handle) != 0;
    if (ordinary && ! flushed) error = { static_cast<int> (GetLastError()), std::system_category() };
    const bool closed = CloseHandle (handle) != 0;
    if (! closed && ! error) error = { static_cast<int> (GetLastError()), std::system_category() };
    return flushed && closed;
#else
    const auto handle = ::open (path.c_str(), O_NOFOLLOW | (directory ? O_RDONLY | O_DIRECTORY : O_RDWR));
    if (handle < 0) { error = { errno, std::generic_category() }; return false; }
    int flushed;
    do { flushed = ::fsync (handle); } while (flushed != 0 && errno == EINTR);
    if (flushed != 0) error = { errno, std::generic_category() };
    const bool closed = ::close (handle) == 0;
    if (! closed && ! error) error = { errno, std::generic_category() };
    return flushed == 0 && closed;
#endif
}

void renameDirectory (const fs::path& source, const fs::path& target, std::error_code& error)
{
#if defined(_WIN32)
    if (! MoveFileExW (dusk::fs::nativeIOPath (source).c_str(), dusk::fs::nativeIOPath (target).c_str(), MOVEFILE_WRITE_THROUGH))
        error = std::error_code (static_cast<int> (GetLastError()), std::system_category());
    else error.clear();
#else
    fs::rename (source, target, error);
#endif
}

bool plainPath (const fs::path& root, const std::string& relative)
{
    std::string error;
    if (! validateRelativePath (relative, error)) return false;
    auto path = root;
    std::error_code ec;
    if (! fs::is_directory (fs::symlink_status (dusk::fs::nativeIOPath (root), ec)) || ec) return false;
    for (const auto& component : fs::u8path (relative))
    {
        path /= component;
        const auto status = fs::symlink_status (dusk::fs::nativeIOPath (path), ec);
        if (ec || fs::is_symlink (status)) return false;
    }
    return fs::is_regular_file (dusk::fs::nativeIOPath (path), ec) && ! ec;
}

std::optional<std::string> receiptText (const fs::path& directory)
{
    if (! plainPath (directory, kReceipt)) return {};
    const auto path = dusk::fs::nativeIOPath (directory / kReceipt);
    std::error_code ec;
    const auto size = fs::file_size (path, ec);
    if (ec || size == 0 || size > 8 * 1024 * 1024) return {};
    std::ifstream stream (path, std::ios::binary);
    std::string text (static_cast<std::size_t> (size), '\0');
    if (! stream.read (text.data(), static_cast<std::streamsize> (text.size()))
        || stream.peek() != std::char_traits<char>::eof() || stream.bad()) return {};
    return text;
}

FailureKind storageFailureKind (const std::error_code& error)
{
    return error == std::errc::no_space_on_device ? FailureKind::DiskFull : FailureKind::None;
}

PackPublishResult failure (const std::string& message, FailureKind kind = FailureKind::None)
{
    PackPublishResult result;
    result.error = message;
    result.failureKind = kind;
    return result;
}

PackPublishResult interrupted (OperationStatus status)
{
    PackPublishResult result;
    result.status = status;
    result.error = "SFZ installation interrupted";
    return result;
}
} // namespace

fs::path defaultPackStoreRoot()
{
    return readLibraryLocations().active.root;
}

std::string PackStore::identity (const ManagedSfzLocator& locator)
{
    if (sodium_init() < 0) return {};
    const auto text = Json::array ({ locator.sourceId, locator.packId, locator.releaseId,
                                    locator.archiveSha256 }).dump();
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest {};
    crypto_hash_sha256 (digest.data(), reinterpret_cast<const unsigned char*> (text.data()), text.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
    sodium_bin2hex (hex.data(), hex.size(), digest.data(), digest.size());
    return hex.data();
}

bool PackStore::initialize (std::string& error, std::error_code* nativeError)
{
    error.clear();
    if (nativeError) nativeError->clear();
    if (storeRoot.empty()) { error = "No SFZ library directory is configured"; return false; }
    if (! requiredMarker.empty())
    {
        if (! libraryLocationAvailable ({ storeRoot, requiredMarker }))
        {
            error = "The SFZ library drive is unavailable. Reconnect it or choose another folder in Settings.";
            return false;
        }
    }
    else
    {
        std::error_code ec;
        fs::create_directories (dusk::fs::nativeIOPath (storeRoot), ec);
        if (! ec) storeRoot = fs::canonical (dusk::fs::nativeIOPath (storeRoot), ec);
        if (! ec) fs::create_directories (dusk::fs::nativeIOPath (storeRoot / "packs"), ec);
        if (ec)
        {
            if (nativeError) *nativeError = ec;
            error = "Cannot open the SFZ library: " + ec.message(); return false;
        }
        if (! fs::is_directory (fs::symlink_status (dusk::fs::nativeIOPath (storeRoot / "packs"), ec)) || ec)
        {
            if (nativeError) *nativeError = ec;
            error = "The SFZ packs directory is not a plain directory"; return false;
        }
    }
    DirectoryLease next;
    if (! next.acquire (storeRoot, DirectoryLease::Access::Shared, error, nativeError)) return false;
    rootLease = std::move (next);
    return true;
}

fs::path PackStore::createStaging (std::string& error, std::error_code* nativeError)
{
    if (! initialize (error, nativeError) || sodium_init() < 0) return {};
    std::array<unsigned char, 16> random {};
    std::array<char, 33> name {};
    randombytes_buf (random.data(), random.size());
    sodium_bin2hex (name.data(), name.size(), random.data(), random.size());
    const auto path = storeRoot / (std::string (".install-") + name.data());
    std::error_code ec;
    if (! fs::create_directory (dusk::fs::nativeIOPath (path), ec))
    {
        if (nativeError) *nativeError = ec;
        error = "Cannot create SFZ staging directory: " + ec.message(); return {};
    }
    return path;
}

PackPublishResult PackStore::publish (const ProviderPack& provider, const std::string& digest,
                                     const fs::path& staging,
                                     const std::vector<std::string>& sfzPaths, const OperationControl& control)
{
    if (const auto status = control.status(); status != OperationStatus::Complete) return interrupted (status);
    std::string error;
    std::error_code ec;
    if (! initialize (error, &ec)) return failure (error, storageFailureKind (ec));
    if (! isSha256 (digest) || ! providerPackFromJson (providerPackToJson (provider)))
        return failure ("Invalid SFZ pack identity or license receipt");
    if (! provider.expectedSha256.empty() && provider.expectedSha256 != digest)
        return failure ("The SFZ archive does not match its published SHA-256", FailureKind::Integrity);
    if (sfzPaths.empty()) return failure ("The archive contains no SFZ instruments");
    if (staging.parent_path() != storeRoot || staging.filename().u8string().rfind (".install-", 0) != 0
        || ! fs::is_directory (fs::symlink_status (dusk::fs::nativeIOPath (staging), ec)) || ec)
        return failure ("The SFZ staging directory is outside its library");
    if (fs::exists (dusk::fs::nativeIOPath (staging / kReceipt), ec) || ec)
        return failure ("The archive uses a reserved SFZ receipt name");
    std::size_t entryCount = 0, licenseCount = 0, licenseBytes = 0;
    bool primaryFound = provider.license.path.empty();
    std::set<std::string> licenseKeys;
    const auto ioStaging = dusk::fs::nativeIOPath (staging);
    for (auto it = fs::recursive_directory_iterator (ioStaging, ec);
         ! ec && it != fs::recursive_directory_iterator(); it.increment (ec))
    {
        if (const auto status = control.status(); status != OperationStatus::Complete) return interrupted (status);
        if (++entryCount > 100000) return failure ("SFZ staging tree exceeds the entry limit");
        const auto relative = it->path().lexically_relative (ioStaging).generic_u8string();
        const auto status = it->symlink_status (ec);
        if (ec || (! fs::is_directory (status) && ! fs::is_regular_file (status)))
            return failure ("SFZ staging tree contains a link or non-regular entry");
        if (fs::is_directory (status))
        {
            if (! validateRelativePath (relative, error)) return failure ("SFZ staging directory path is unsafe");
            continue;
        }
        // SFZ Instruments packs install whatever licensing documents they carry; only the resolved file is checked.
        if (relative != provider.license.path
            && (provider.sourceId == "sfzinstruments" || ! isPackLicenseDocument (relative))) continue;
        const auto key = portablePathKey (relative);
        if (++licenseCount > kMaxLicenseDocuments || ! key || ! licenseKeys.insert (*key).second
            || ! plainPath (staging, relative)) return failure ("SFZ licensing paths are unsafe, ambiguous, or too numerous");
        const auto size = fs::file_size (it->path(), ec);
        if (ec || size == 0 || size > kMaxLicenseBytes || size > kMaxCombinedLicenseBytes - licenseBytes)
            return failure ("SFZ licensing documents exceed their size limits");
        std::ifstream stream (it->path(), std::ios::binary);
        std::string text (static_cast<std::size_t> (size), '\0');
        if (! stream.read (text.data(), static_cast<std::streamsize> (text.size()))
            || stream.peek() != std::char_traits<char>::eof() || stream.bad())
            return failure ("Cannot read complete SFZ licensing document");
        const auto checked = validatePackLicenseDocument (provider.license, relative, text);
        if (! checked) return failure (checked.error + ": " + relative);
        licenseBytes += text.size();
        if (relative == provider.license.path) primaryFound = true;
    }
    if (ec || ! primaryFound) return failure ("SFZ licensing documents are missing or unreadable");
    InstalledPack pack;
    pack.provider = provider;
    pack.archiveSha256 = digest;
    Json entries = Json::array();
    std::set<std::string> paths, ids;
    for (const auto& relative : sfzPaths)
    {
        const auto key = portablePathKey (relative);
        if (! plainPath (staging, relative)
            || dusk::text::toLowerCase (fs::u8path (relative).extension().u8string()) != ".sfz"
            || ! key || ! paths.insert (*key).second)
            return failure ("Invalid or missing SFZ entrypoint in the extracted archive");
        ProviderInstrument instrument { relative, fs::u8path (relative).stem().u8string(), relative, true };
        const auto known = std::find_if (provider.instruments.begin(), provider.instruments.end(),
            [&relative] (const ProviderInstrument& entry) { return entry.relativePath == relative; });
        if (known != provider.instruments.end()) instrument = *known;
        if (instrument.id.empty() || ! ids.insert (instrument.id).second)
            return failure ("Duplicate SFZ instrument identity");
        pack.instruments.push_back (instrument);
        entries.push_back ({ { "id", instrument.id }, { "name", instrument.name },
                            { "path", relative }, { "candidate", instrument.candidateOnly } });
    }
    const auto contents = hashPackContents (staging, control);
    if (! contents)
    {
        auto result = failure (contents.error, contents.failureKind);
        result.status = contents.status;
        return result;
    }
    pack.installedTreeSha256 = contents.sha256;
    const Json receipt { { "schema", 1 }, { "provider", Json::parse (providerPackToJson (provider)) },
                         { "presentation", { { "artwork_url", provider.presentation.artworkUrl },
                                               { "category_hint", provider.presentation.categoryHint } } },
                         { "archive_sha256", digest }, { "installed_tree_sha256_v1", contents.sha256 },
                         { "instruments", std::move (entries) } };
    if (const auto status = control.status(); status != OperationStatus::Complete) return interrupted (status);
    if (! dusk::fs::writeStringToFile (staging / kReceipt, receipt.dump (2), &ec)
        || ! syncPath (staging / kReceipt, false, ec))
        return failure ("Cannot write SFZ installation receipt", storageFailureKind (ec));
   #if ! defined(_WIN32)
    for (auto it = fs::recursive_directory_iterator (staging, ec);
         ! ec && it != fs::recursive_directory_iterator(); it.increment (ec))
        if (fs::is_directory (it->symlink_status (ec)) && ! ec && ! syncPath (it->path(), true, ec))
            return failure ("Cannot flush SFZ staging directory", storageFailureKind (ec));
    if (ec || ! syncPath (staging, true, ec)) return failure ("Cannot flush SFZ staging directory", storageFailureKind (ec));
   #endif
    if (const auto status = control.status(); status != OperationStatus::Complete) return interrupted (status);
    pack.directory = storeRoot / "packs" / identity (pack.locatorFor (pack.instruments.front()));
    fs::path displaced;
    if (fs::exists (dusk::fs::nativeIOPath (pack.directory), ec))
    {
        auto existing = readReceipt (pack.directory);
        if (existing)
        {
            fs::remove_all (ioStaging, ec);
            PackPublishResult result;
            result.status = OperationStatus::Complete;
            result.pack = std::move (existing);
            return result;
        }
        if (! fs::is_directory (fs::symlink_status (dusk::fs::nativeIOPath (pack.directory), ec)) || ec)
            return failure ("The SFZ installation path is not a plain directory");
        displaced = staging;
        displaced += ".replaced";
        if (fs::exists (fs::symlink_status (dusk::fs::nativeIOPath (displaced), ec)))
            return failure ("The SFZ repair directory is already occupied");
        renameDirectory (pack.directory, displaced, ec);
        if (ec) return failure ("Cannot repair SFZ installation: " + ec.message(), storageFailureKind (ec));
    }
    renameDirectory (staging, pack.directory, ec);
    if (ec)
    {
        const auto kind = storageFailureKind (ec);
        const auto error = "Cannot publish SFZ installation: " + ec.message();
        if (! displaced.empty())
        {
            renameDirectory (displaced, pack.directory, ec);
            if (ec) return failure (error + "; previous files remain in " + displaced.u8string(), kind);
        }
        return failure (error, kind);
    }
    if (! syncPath (pack.directory.parent_path(), true, ec) || ! syncPath (storeRoot, true, ec))
        return failure ("Cannot flush SFZ installation directory", storageFailureKind (ec));
    if (! displaced.empty()) fs::remove_all (dusk::fs::nativeIOPath (displaced), ec);
    pack.programs = indexPackPrograms (pack.directory, pack.instruments);
    PackPublishResult result;
    result.status = OperationStatus::Complete;
    result.pack = std::move (pack);
    return result;
}

std::optional<InstalledPack> PackStore::readReceipt (const fs::path& directory, bool requireEntrypoints,
                                                  std::string* receiptBytes) const
{
    if (directory.parent_path() != storeRoot / "packs") return {};
    const auto bytes = receiptText (directory);
    if (! bytes) return {};
    const auto receipt = Json::parse (*bytes, nullptr, false);
    if (receipt.is_discarded() || dusk::json::getInt (receipt, "schema", 0) != 1) return {};
    auto parsed = providerPackFromJson (dusk::json::child (receipt, "provider").dump(), PackDescriptorUse::InstalledReceipt);
    if (! parsed || ! parsed.pack) return {};
    InstalledPack pack;
    pack.provider = std::move (*parsed.pack);
    const auto& presentation = dusk::json::child (receipt, "presentation");
    const auto artwork = dusk::json::getString (presentation, "artwork_url");
    if (artwork.size() <= 2048 && artwork.compare (0, 8, "https://") == 0
        && artwork.find_first_of ("\r\n\t @\\") == std::string::npos && artwork.find ('\0') == std::string::npos)
        pack.provider.presentation.artworkUrl = artwork;
    const auto category = dusk::json::getString (presentation, "category_hint");
    if (category.size() <= 8192 && category.find ('\0') == std::string::npos)
        pack.provider.presentation.categoryHint = category;
    pack.archiveSha256 = dusk::json::getString (receipt, "archive_sha256");
    if (receipt.contains ("installed_tree_sha256_v1"))
    {
        const auto& digest = receipt["installed_tree_sha256_v1"];
        if (! digest.is_string() || ! isSha256 (digest.get<std::string>())) return {};
        pack.installedTreeSha256 = digest.get<std::string>();
    }
    pack.directory = directory;
    if (! isSha256 (pack.archiveSha256)
        || (! pack.provider.expectedSha256.empty()
            && pack.provider.expectedSha256 != pack.archiveSha256)) return {};
    std::set<std::string> paths, ids;
    for (const auto& entry : dusk::json::array (receipt, "instruments"))
    {
        ProviderInstrument instrument { dusk::json::getString (entry, "id"),
            dusk::json::getString (entry, "name"), dusk::json::getString (entry, "path"),
            dusk::json::getBool (entry, "candidate", true) };
        const auto key = portablePathKey (instrument.relativePath);
        std::string error;
        if (instrument.id.empty() || instrument.id.size() > 512
            || ! validateRelativePath (instrument.relativePath, error)
            || (requireEntrypoints && ! plainPath (directory, instrument.relativePath))
            || dusk::text::toLowerCase (fs::u8path (instrument.relativePath).extension().u8string()) != ".sfz"
            || ! key || ! paths.insert (*key).second || ! ids.insert (instrument.id).second) return {};
        pack.instruments.push_back (std::move (instrument));
    }
    if (pack.instruments.empty()
        || directory.filename().u8string() != identity (pack.locatorFor (pack.instruments.front()))) return {};
    if (receiptBytes) *receiptBytes = *bytes;
    return pack;
}

OperationResult PackStore::verify (const InstalledPack& selected, const OperationControl& control) const
{
    if (const auto status = control.status(); status != OperationStatus::Complete)
        return { status, "SFZ file verification interrupted" };
    if (selected.instruments.empty()
        || selected.directory != storeRoot / "packs" / identity (selected.locatorFor (selected.instruments.front())))
        return { OperationStatus::Failed, "Invalid installed SFZ pack selection" };
    if (! libraryLocationAvailable ({ storeRoot, requiredMarker }))
        return { OperationStatus::Failed, "The SFZ library drive is unavailable" };
    std::string bytes;
    const auto pack = readReceipt (selected.directory, false, &bytes);
    if (! pack) return { OperationStatus::Failed, "The SFZ installation receipt is invalid", FailureKind::Integrity };
    if (pack->installedTreeSha256.empty())
        return { OperationStatus::Failed, "File verification is unavailable: this installation has no recorded file digest" };
    DirectoryLease lease;
    std::string error;
    if (! lease.acquire (storeRoot, DirectoryLease::Access::Shared, error))
        return { OperationStatus::Failed, error };
    if (! libraryLocationAvailable ({ storeRoot, requiredMarker }) || receiptText (selected.directory) != bytes)
        return { OperationStatus::Failed, "The SFZ installation changed before verification", FailureKind::Integrity };
    const auto contents = hashPackContents (pack->directory, control);
    if (! contents) return contents;
    if (! libraryLocationAvailable ({ storeRoot, requiredMarker }) || receiptText (selected.directory) != bytes)
        return { OperationStatus::Failed, "The SFZ installation changed during verification", FailureKind::Integrity };
    if (contents.sha256 != pack->installedTreeSha256)
        return { OperationStatus::Failed, "Installed SFZ files differ from the installation record", FailureKind::Integrity };
    if (const auto status = control.status(); status != OperationStatus::Complete)
        return { status, "SFZ file verification interrupted" };
    return { OperationStatus::Complete, {} };
}

std::vector<InstalledPack> PackStore::installed() const
{
    std::vector<InstalledPack> result;
    std::error_code ec;
    for (auto it = fs::directory_iterator (dusk::fs::nativeIOPath (storeRoot / "packs"), ec);
         ! ec && it != fs::directory_iterator(); it.increment (ec))
        if (auto pack = readReceipt (storeRoot / "packs" / it->path().filename()))
        {
            pack->programs = indexPackPrograms (pack->directory, pack->instruments);
            result.push_back (std::move (*pack));
        }
    std::sort (result.begin(), result.end(), [] (const InstalledPack& a, const InstalledPack& b)
        { return a.provider.displayName < b.provider.displayName; });
    return result;
}

std::optional<fs::path> PackStore::resolve (const ManagedSfzLocator& locator) const
{
    if (! locatorFromJson (locatorToJson (locator))) return {};
    const auto pack = readReceipt (storeRoot / "packs" / identity (locator));
    if (! pack) return {};
    for (const auto& instrument : pack->instruments)
        if (pack->locatorFor (instrument) == locator)
            return pack->directory / fs::u8path (instrument.relativePath);
    return {};
}

std::optional<ManagedSfzLocator> PackStore::locatorForFile (const fs::path& file) const
{
    std::error_code ec;
    const auto actual = fs::canonical (dusk::fs::nativeIOPath (file), ec);
    if (ec) return {};
    const auto packs = fs::canonical (dusk::fs::nativeIOPath (storeRoot / "packs"), ec);
    if (ec) return {};
    const auto relative = actual.lexically_relative (packs);
    if (relative.empty()) return {};
    const auto key = relative.begin()->u8string();
    if (! isSha256 (key)) return {};
    const auto pack = readReceipt (storeRoot / "packs" / key);
    if (! pack) return {};
    for (const auto& instrument : pack->instruments)
        if (fs::equivalent (dusk::fs::nativeIOPath (actual),
                            dusk::fs::nativeIOPath (pack->directory / fs::u8path (instrument.relativePath)), ec) && ! ec)
            return pack->locatorFor (instrument);
    return {};
}

OperationResult PackStore::remove (const InstalledPack& pack, const std::vector<ManagedSfzLocator>& inUse)
{
    OperationResult result;
    if (pack.instruments.empty()) { result.error = "Invalid installed pack"; return result; }
    const auto key = identity (pack.locatorFor (pack.instruments.front()));
    if (std::any_of (inUse.begin(), inUse.end(), [&key] (const ManagedSfzLocator& locator)
        { return identity (locator) == key; }))
    { result.error = "This SFZ pack is used by the current session"; return result; }
    const auto directory = storeRoot / "packs" / key;
    if (directory != pack.directory || ! readReceipt (directory))
    { result.error = "Cannot remove an invalid SFZ installation"; return result; }
    std::error_code ec;
    fs::remove_all (dusk::fs::nativeIOPath (directory), ec);
    if (ec) { result.error = "Cannot remove SFZ installation: " + ec.message(); return result; }
    result.status = OperationStatus::Complete;
    return result;
}
} // namespace duskstudio::sfz

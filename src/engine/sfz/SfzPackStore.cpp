#include "SfzPackStore.h"
#include "../../foundation/Fs.h"
#include "../../foundation/AppConfigDir.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <set>
#include <system_error>

namespace duskstudio::sfz
{
namespace
{
namespace fs = std::filesystem;
using Json = dusk::json::Json;
constexpr const char* kReceipt = ".dusk-pack.json";

bool plainPath (const fs::path& root, const std::string& relative)
{
    std::string error;
    if (! validateRelativePath (relative, error)) return false;
    auto path = root;
    std::error_code ec;
    if (! fs::is_directory (fs::symlink_status (root, ec)) || ec) return false;
    for (const auto& component : fs::u8path (relative))
    {
        path /= component;
        const auto status = fs::symlink_status (path, ec);
        if (ec || fs::is_symlink (status)) return false;
    }
    return fs::is_regular_file (path, ec) && ! ec;
}

PackPublishResult failure (const std::string& message)
{
    PackPublishResult result;
    result.error = message;
    return result;
}
} // namespace

fs::path defaultPackStoreRoot()
{
    const auto config = dusk::fs::appConfigDir();
    return config.empty() ? fs::path {} : config / "sfz-library";
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

bool PackStore::initialize (std::string& error)
{
    if (storeRoot.empty()) { error = "No SFZ library directory is configured"; return false; }
    std::error_code ec;
    fs::create_directories (storeRoot, ec);
    if (! ec) storeRoot = fs::canonical (storeRoot, ec);
    if (! ec) fs::create_directories (storeRoot / "packs", ec);
    if (ec) { error = "Cannot open the SFZ library: " + ec.message(); return false; }
    if (! fs::is_directory (fs::symlink_status (storeRoot / "packs", ec)) || ec)
    { error = "The SFZ packs directory is not a plain directory"; return false; }
    return true;
}

fs::path PackStore::createStaging (std::string& error)
{
    if (! initialize (error) || sodium_init() < 0) return {};
    std::array<unsigned char, 16> random {};
    std::array<char, 33> name {};
    randombytes_buf (random.data(), random.size());
    sodium_bin2hex (name.data(), name.size(), random.data(), random.size());
    const auto path = storeRoot / (std::string (".install-") + name.data());
    std::error_code ec;
    if (! fs::create_directory (path, ec))
    { error = "Cannot create SFZ staging directory: " + ec.message(); return {}; }
    return path;
}

PackPublishResult PackStore::publish (const ProviderPack& provider, const std::string& digest,
                                     const fs::path& staging,
                                     const std::vector<std::string>& sfzPaths)
{
    std::string error;
    if (! initialize (error)) return failure (error);
    if (! isSha256 (digest) || ! providerPackFromJson (providerPackToJson (provider)))
        return failure ("Invalid SFZ pack identity or Creative Commons license receipt");
    if (! provider.expectedSha256.empty() && provider.expectedSha256 != digest)
        return failure ("The SFZ archive does not match its published SHA-256");
    if (sfzPaths.empty()) return failure ("The archive contains no SFZ instruments");
    std::error_code ec;
    if (staging.parent_path() != storeRoot || staging.filename().u8string().rfind (".install-", 0) != 0
        || ! fs::is_directory (fs::symlink_status (staging, ec)) || ec)
        return failure ("The SFZ staging directory is outside its library");
    if (fs::exists (staging / kReceipt, ec) || ec)
        return failure ("The archive uses a reserved SFZ receipt name");
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
    const Json receipt { { "schema", 1 }, { "provider", Json::parse (providerPackToJson (provider)) },
                         { "presentation", { { "artwork_url", provider.presentation.artworkUrl },
                                               { "category_hint", provider.presentation.categoryHint } } },
                         { "archive_sha256", digest }, { "instruments", std::move (entries) } };
    if (! dusk::fs::writeStringToFile (staging / kReceipt, receipt.dump (2)))
        return failure ("Cannot write SFZ installation receipt");
    pack.directory = storeRoot / "packs" / identity (pack.locatorFor (pack.instruments.front()));
    if (fs::exists (pack.directory, ec))
    {
        auto existing = readReceipt (pack.directory);
        if (! existing) return failure ("An incomplete SFZ installation already occupies this version");
        fs::remove_all (staging, ec);
        PackPublishResult result;
        result.status = OperationStatus::Complete;
        result.pack = std::move (existing);
        return result;
    }
    fs::rename (staging, pack.directory, ec);
    if (ec) return failure ("Cannot publish SFZ installation: " + ec.message());
    PackPublishResult result;
    result.status = OperationStatus::Complete;
    result.pack = std::move (pack);
    return result;
}

std::optional<InstalledPack> PackStore::readReceipt (const fs::path& directory) const
{
    if (directory.parent_path() != storeRoot / "packs" || ! plainPath (directory, kReceipt)) return {};
    if (dusk::fs::fileSize (directory / kReceipt) > 8 * 1024 * 1024) return {};
    const auto receipt = Json::parse (dusk::fs::loadFileAsString (directory / kReceipt), nullptr, false);
    if (receipt.is_discarded() || dusk::json::getInt (receipt, "schema", 0) != 1) return {};
    auto parsed = providerPackFromJson (dusk::json::child (receipt, "provider").dump());
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
        if (instrument.id.empty() || instrument.id.size() > 512
            || ! plainPath (directory, instrument.relativePath)
            || dusk::text::toLowerCase (fs::u8path (instrument.relativePath).extension().u8string()) != ".sfz"
            || ! key || ! paths.insert (*key).second || ! ids.insert (instrument.id).second) return {};
        pack.instruments.push_back (std::move (instrument));
    }
    if (pack.instruments.empty()
        || directory.filename().u8string() != identity (pack.locatorFor (pack.instruments.front()))) return {};
    return pack;
}

std::vector<InstalledPack> PackStore::installed() const
{
    std::vector<InstalledPack> result;
    std::error_code ec;
    for (auto it = fs::directory_iterator (storeRoot / "packs", ec);
         ! ec && it != fs::directory_iterator(); it.increment (ec))
        if (auto pack = readReceipt (it->path())) result.push_back (std::move (*pack));
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
    const auto actual = fs::canonical (file, ec);
    if (ec) return {};
    const auto relative = actual.lexically_relative (storeRoot / "packs");
    if (relative.empty()) return {};
    const auto key = relative.begin()->u8string();
    if (! isSha256 (key)) return {};
    const auto pack = readReceipt (storeRoot / "packs" / key);
    if (! pack) return {};
    for (const auto& instrument : pack->instruments)
        if (fs::equivalent (actual, pack->directory / fs::u8path (instrument.relativePath), ec) && ! ec)
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
    fs::remove_all (directory, ec);
    if (ec) { result.error = "Cannot remove SFZ installation: " + ec.message(); return result; }
    result.status = OperationStatus::Complete;
    return result;
}
} // namespace duskstudio::sfz

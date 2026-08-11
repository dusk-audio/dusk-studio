#include "SfzCatalog.h"

#include "../../foundation/Json.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace duskstudio::sfz
{
namespace
{
using Json = dusk::json::Json;

constexpr std::uint32_t kSchemaVersion = 1;
constexpr std::size_t kMaxPayloadBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxCatalogIdLength = 64;
constexpr std::size_t kMaxStableIdLength = 64;
constexpr std::size_t kMaxReleaseIdLength = 128;
constexpr std::size_t kMaxNameLength = 256;
constexpr std::size_t kMaxUrlLength = 2048;
constexpr std::size_t kMaxTimestampLength = 64;
constexpr std::size_t kMaxPathLength = 512;
constexpr std::size_t kMaxPathDepth = 32;
constexpr std::size_t kMaxPacks = 4096;
constexpr std::size_t kMaxInstrumentsPerPack = 4096;
constexpr std::size_t kMaxTagsPerPack = 64;
constexpr std::size_t kMaxTagLength = 64;
constexpr std::uint64_t kMaxCompressedBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxExpandedBytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kMaxFilesPerPack = 100000;

bool isAsciiAlpha (unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isAsciiDigit (unsigned char c)
{
    return c >= '0' && c <= '9';
}

bool isAsciiAlphaNumeric (unsigned char c)
{
    return isAsciiAlpha (c) || isAsciiDigit (c);
}

bool isLowerHex (unsigned char c)
{
    return isAsciiDigit (c) || (c >= 'a' && c <= 'f');
}

bool equalsAsciiCaseInsensitive (std::string_view value, std::string_view expected)
{
    if (value.size() != expected.size())
        return false;
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        auto c = static_cast<unsigned char> (value[i]);
        if (c >= 'a' && c <= 'z')
            c = static_cast<unsigned char> (c - 'a' + 'A');
        if (c != static_cast<unsigned char> (expected[i]))
            return false;
    }
    return true;
}

bool isWindowsReservedBasename (std::string_view segment)
{
    const auto dot = segment.find ('.');
    const auto basename = segment.substr (0, dot);
    if (equalsAsciiCaseInsensitive (basename, "CON")
        || equalsAsciiCaseInsensitive (basename, "PRN")
        || equalsAsciiCaseInsensitive (basename, "AUX")
        || equalsAsciiCaseInsensitive (basename, "NUL"))
        return true;

    if (basename.size() < 4)
        return false;
    if (! equalsAsciiCaseInsensitive (basename.substr (0, 3), "COM")
        && ! equalsAsciiCaseInsensitive (basename.substr (0, 3), "LPT"))
        return false;

    const auto suffix = basename.substr (3);
    if (suffix.size() == 1)
        return suffix[0] >= '1' && suffix[0] <= '9';
    return suffix.size() == 2
        && static_cast<unsigned char> (suffix[0]) == 0xc2
        && (static_cast<unsigned char> (suffix[1]) == 0xb9
            || static_cast<unsigned char> (suffix[1]) == 0xb2
            || static_cast<unsigned char> (suffix[1]) == 0xb3);
}

struct Parser
{
    std::string error;

    bool fail (std::string path, std::string reason)
    {
        error = std::move (path) + ": " + std::move (reason);
        return false;
    }

    bool validateKeys (const Json& object, const std::string& path,
                       std::initializer_list<std::string_view> allowed)
    {
        for (const auto& item : object.items())
        {
            const auto known = std::any_of (allowed.begin(), allowed.end(),
                [&item] (std::string_view key) { return item.key() == key; });
            if (! known)
                return fail (path + "." + item.key(), "unknown field");
        }
        return true;
    }

    const Json* required (const Json& object, const char* key,
                          const std::string& path)
    {
        const auto it = object.find (key);
        if (it == object.end())
        {
            fail (path + "." + key, "required field is missing");
            return nullptr;
        }
        return &*it;
    }

    bool readString (const Json& object, const char* key, const std::string& path,
                     std::string& out, std::size_t maxLength)
    {
        const auto* value = required (object, key, path);
        if (value == nullptr)
            return false;
        if (! value->is_string())
            return fail (path + "." + key, "expected a string");

        out = value->get<std::string>();
        if (out.empty())
            return fail (path + "." + key, "must not be empty");
        if (out.size() > maxLength)
            return fail (path + "." + key, "exceeds the length limit");
        if (std::any_of (out.begin(), out.end(), [] (unsigned char c)
            { return c == 0 || c < 0x20 || c == 0x7f; }))
            return fail (path + "." + key, "contains a control character");
        return true;
    }

    bool readPositiveInteger (const Json& object, const char* key,
                              const std::string& path, std::uint64_t maximum,
                              std::uint64_t& out)
    {
        const auto* value = required (object, key, path);
        if (value == nullptr)
            return false;
        if (! value->is_number_integer() && ! value->is_number_unsigned())
            return fail (path + "." + key, "expected a positive integer");

        std::uint64_t candidate = 0;
        if (value->is_number_unsigned())
        {
            candidate = value->get<std::uint64_t>();
        }
        else
        {
            const auto signedCandidate = value->get<std::int64_t>();
            if (signedCandidate <= 0)
                return fail (path + "." + key, "must be greater than zero");
            candidate = static_cast<std::uint64_t> (signedCandidate);
        }

        if (candidate == 0)
            return fail (path + "." + key, "must be greater than zero");
        if (candidate > maximum)
            return fail (path + "." + key, "exceeds the supported limit");
        out = candidate;
        return true;
    }

    bool readTrue (const Json& object, const char* key, const std::string& path,
                   bool& out)
    {
        const auto* value = required (object, key, path);
        if (value == nullptr)
            return false;
        if (! value->is_boolean())
            return fail (path + "." + key, "expected a boolean");
        if (! value->get<bool>())
            return fail (path + "." + key, "must be true");
        out = true;
        return true;
    }

    bool validateStableId (const std::string& value, const std::string& path,
                           std::size_t maxLength = kMaxStableIdLength)
    {
        if (value.size() > maxLength)
            return fail (path, "exceeds the identifier length limit");

        const auto isLowerAlphaNumeric = [] (unsigned char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        };
        if (! isLowerAlphaNumeric (static_cast<unsigned char> (value.front()))
            || ! isLowerAlphaNumeric (static_cast<unsigned char> (value.back())))
            return fail (path, "must start and end with a lowercase letter or digit");

        for (const auto c : value)
        {
            const auto byte = static_cast<unsigned char> (c);
            if (! isLowerAlphaNumeric (byte) && c != '-' && c != '_' && c != '.')
                return fail (path, "contains a character outside [a-z0-9._-]");
        }
        return true;
    }

    bool validateReleaseId (const std::string& value, const std::string& path)
    {
        if (value.size() > kMaxReleaseIdLength)
            return fail (path, "exceeds the release identifier length limit");

        if (! isAsciiAlphaNumeric (static_cast<unsigned char> (value.front()))
            || ! isAsciiAlphaNumeric (static_cast<unsigned char> (value.back())))
            return fail (path, "must start and end with an ASCII letter or digit");

        for (const auto c : value)
        {
            const auto byte = static_cast<unsigned char> (c);
            if (! isAsciiAlphaNumeric (byte)
                && c != '-' && c != '_' && c != '.' && c != '+')
                return fail (path, "contains an unsupported character");
        }
        return true;
    }

    bool validateHttpsUrl (const std::string& value, const std::string& path)
    {
        constexpr std::string_view scheme { "https://" };
        if (value.compare (0, scheme.size(), scheme) != 0)
            return fail (path, "must use HTTPS");
        if (value.size() == scheme.size() || value[scheme.size()] == '/')
            return fail (path, "must include a host");

        const auto hostEnd = value.find_first_of ("/?#", scheme.size());
        const auto host = value.substr (scheme.size(), hostEnd - scheme.size());
        if (host.empty() || host.size() > 253)
            return fail (path, "has an invalid DNS host length");

        std::size_t begin = 0;
        while (begin <= host.size())
        {
            const auto end = host.find ('.', begin);
            const auto length = (end == std::string::npos ? host.size() : end) - begin;
            if (length == 0 || length > 63)
                return fail (path, "has an invalid DNS label length");

            const auto label = std::string_view (host).substr (begin, length);
            if (! isAsciiAlphaNumeric (static_cast<unsigned char> (label.front()))
                || ! isAsciiAlphaNumeric (static_cast<unsigned char> (label.back())))
                return fail (path, "DNS labels must have alphanumeric edges");
            if (! std::all_of (label.begin(), label.end(), [] (unsigned char c)
                { return isAsciiAlphaNumeric (c) || c == '-'; }))
                return fail (path, "DNS labels must contain only ASCII alphanumerics or hyphens");

            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        return true;
    }

    bool validateSha256 (const std::string& value, const std::string& path)
    {
        if (value.size() != 64)
            return fail (path, "must contain exactly 64 lowercase hexadecimal digits");
        if (! std::all_of (value.begin(), value.end(), isLowerHex))
            return fail (path, "must contain only lowercase hexadecimal digits");
        return true;
    }

    bool validateGitObjectId (const std::string& value, const std::string& path)
    {
        if (value.size() != 40 && value.size() != 64)
            return fail (path, "must contain exactly 40 or 64 hexadecimal digits");
        if (! std::all_of (value.begin(), value.end(), isLowerHex))
            return fail (path, "must contain only lowercase hexadecimal digits");
        return true;
    }

    bool validateRelativePath (const std::string& value, const std::string& path)
    {
        if (value.size() > kMaxPathLength)
            return fail (path, "exceeds the path length limit");
        if (value.front() == '/' || value.find ('\\') != std::string::npos)
            return fail (path, "must be a forward-slash relative path");
        if (value.find_first_of (":<>\"|?*") != std::string::npos)
            return fail (path, "contains a Win32-invalid filename character");

        std::size_t depth = 0;
        std::size_t begin = 0;
        while (begin <= value.size())
        {
            const auto end = value.find ('/', begin);
            const auto length = (end == std::string::npos ? value.size() : end) - begin;
            if (length == 0)
                return fail (path, "must not contain empty path segments");

            const auto segment = value.substr (begin, length);
            if (segment == "." || segment == "..")
                return fail (path, "must not contain dot path segments");
            if (segment.back() == '.' || segment.back() == ' ')
                return fail (path, "path segments must not end in a dot or space");
            if (std::any_of (segment.begin(), segment.end(), [] (unsigned char c)
                { return c == 0 || c < 0x20 || c == 0x7f; }))
                return fail (path, "contains a control character");
            if (isWindowsReservedBasename (segment))
                return fail (path, "contains a reserved Windows device basename");

            if (++depth > kMaxPathDepth)
                return fail (path, "exceeds the path depth limit");
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        return true;
    }

    bool parseLicense (const Json& object, const std::string& path,
                       CatalogLicense& out)
    {
        if (! object.is_object())
            return fail (path, "expected an object");
        if (! validateKeys (object, path,
                            { "spdx", "source_url", "file", "file_sha256",
                              "redistribution_allowed", "mirror_allowed" }))
            return false;
        if (! readString (object, "spdx", path, out.spdx, 32)
            || ! readString (object, "source_url", path, out.sourceUrl, kMaxUrlLength)
            || ! readString (object, "file", path, out.file, kMaxPathLength)
            || ! readString (object, "file_sha256", path, out.fileSha256, 64)
            || ! readTrue (object, "redistribution_allowed", path,
                           out.redistributionAllowed)
            || ! readTrue (object, "mirror_allowed", path, out.mirrorAllowed))
            return false;

        if (out.spdx != "CC0-1.0")
            return fail (path + ".spdx", "license is not allowed by catalog schema v1");
        return validateHttpsUrl (out.sourceUrl, path + ".source_url")
            && validateRelativePath (out.file, path + ".file")
            && validateSha256 (out.fileSha256, path + ".file_sha256");
    }

    bool parseInstrument (const Json& object, const std::string& path,
                          CatalogInstrument& out)
    {
        if (! object.is_object())
            return fail (path, "expected an object");
        if (! validateKeys (object, path, { "id", "name", "relative_entrypoint" }))
            return false;
        if (! readString (object, "id", path, out.id, kMaxStableIdLength)
            || ! readString (object, "name", path, out.name, kMaxNameLength)
            || ! readString (object, "relative_entrypoint", path,
                             out.relativeEntrypoint, kMaxPathLength))
            return false;
        if (! validateStableId (out.id, path + ".id")
            || ! validateRelativePath (out.relativeEntrypoint,
                                       path + ".relative_entrypoint"))
            return false;

        const auto hasSupportedExtension =
            out.relativeEntrypoint.size() >= 4
            && (out.relativeEntrypoint.compare (
                    out.relativeEntrypoint.size() - 4, 4, ".sfz") == 0
                || out.relativeEntrypoint.compare (
                    out.relativeEntrypoint.size() - 4, 4, ".sf2") == 0);
        return hasSupportedExtension
            || fail (path + ".relative_entrypoint", "must end in .sfz or .sf2");
    }

    bool parseTags (const Json& object, const std::string& path,
                    std::vector<std::string>& out)
    {
        const auto it = object.find ("tags");
        if (it == object.end())
            return true;
        if (! it->is_array())
            return fail (path + ".tags", "expected an array");
        if (it->size() > kMaxTagsPerPack)
            return fail (path + ".tags", "contains too many tags");

        std::unordered_set<std::string> seen;
        out.reserve (it->size());
        for (std::size_t i = 0; i < it->size(); ++i)
        {
            const auto& value = (*it)[i];
            const auto itemPath = path + ".tags[" + std::to_string (i) + "]";
            if (! value.is_string())
                return fail (itemPath, "expected a string");
            auto tag = value.get<std::string>();
            if (tag.empty() || tag.size() > kMaxTagLength)
                return fail (itemPath, "has an invalid length");
            if (! validateStableId (tag, itemPath, kMaxTagLength))
                return false;
            if (! seen.insert (tag).second)
                return fail (itemPath, "duplicates an earlier tag");
            out.push_back (std::move (tag));
        }
        return true;
    }

    bool parseOptionalYanked (const Json& object, const std::string& path, bool& out)
    {
        const auto it = object.find ("yanked");
        if (it == object.end())
            return true;
        if (! it->is_boolean())
            return fail (path + ".yanked", "expected a boolean");
        out = it->get<bool>();
        return true;
    }

    bool parsePack (const Json& object, const std::string& path, CatalogPack& out)
    {
        if (! object.is_object())
            return fail (path, "expected an object");
        if (! validateKeys (object, path,
                            { "id", "release_id", "display_name", "author",
                              "source_url", "source_revision", "download_url",
                              "archive_sha256", "archive_format", "expected_root",
                              "compressed_bytes", "expanded_bytes", "max_files",
                              "license", "instruments", "tags", "yanked" }))
            return false;
        if (! readString (object, "id", path, out.id, kMaxStableIdLength)
            || ! readString (object, "release_id", path, out.releaseId,
                             kMaxReleaseIdLength)
            || ! readString (object, "display_name", path, out.displayName,
                             kMaxNameLength)
            || ! readString (object, "author", path, out.author, kMaxNameLength)
            || ! readString (object, "source_url", path, out.sourceUrl, kMaxUrlLength)
            || ! readString (object, "source_revision", path, out.sourceRevision, 64)
            || ! readString (object, "download_url", path, out.downloadUrl,
                             kMaxUrlLength)
            || ! readString (object, "archive_sha256", path, out.archiveSha256, 64)
            || ! readString (object, "archive_format", path, out.archiveFormat, 16)
            || ! readString (object, "expected_root", path, out.expectedRoot,
                             kMaxPathLength))
            return false;

        std::uint64_t maxFiles = 0;
        if (! readPositiveInteger (object, "compressed_bytes", path,
                                   kMaxCompressedBytes, out.compressedBytes)
            || ! readPositiveInteger (object, "expanded_bytes", path,
                                      kMaxExpandedBytes, out.expandedBytes)
            || ! readPositiveInteger (object, "max_files", path,
                                      kMaxFilesPerPack, maxFiles))
            return false;
        out.maxFiles = static_cast<std::uint32_t> (maxFiles);

        if (! validateStableId (out.id, path + ".id")
            || ! validateReleaseId (out.releaseId, path + ".release_id")
            || ! validateHttpsUrl (out.sourceUrl, path + ".source_url")
            || ! validateGitObjectId (out.sourceRevision, path + ".source_revision")
            || ! validateHttpsUrl (out.downloadUrl, path + ".download_url")
            || ! validateSha256 (out.archiveSha256, path + ".archive_sha256")
            || ! validateRelativePath (out.expectedRoot, path + ".expected_root"))
            return false;
        if (out.archiveFormat != "zip")
            return fail (path + ".archive_format", "must be zip");
        if (out.expectedRoot.find ('/') != std::string::npos)
            return fail (path + ".expected_root", "must contain exactly one segment");
        if (out.expectedRoot != out.id)
            return fail (path + ".expected_root", "must equal the pack id");

        const auto* license = required (object, "license", path);
        if (license == nullptr || ! parseLicense (*license, path + ".license", out.license))
            return false;

        const auto* instruments = required (object, "instruments", path);
        if (instruments == nullptr)
            return false;
        if (! instruments->is_array())
            return fail (path + ".instruments", "expected an array");
        if (instruments->empty())
            return fail (path + ".instruments", "must not be empty");
        if (instruments->size() > kMaxInstrumentsPerPack)
            return fail (path + ".instruments", "contains too many instruments");

        std::unordered_set<std::string> instrumentIds;
        out.instruments.reserve (instruments->size());
        for (std::size_t i = 0; i < instruments->size(); ++i)
        {
            const auto itemPath = path + ".instruments[" + std::to_string (i) + "]";
            CatalogInstrument instrument;
            if (! parseInstrument ((*instruments)[i], itemPath, instrument))
                return false;
            if (! instrumentIds.insert (instrument.id).second)
                return fail (itemPath + ".id", "duplicates an instrument identity");
            out.instruments.push_back (std::move (instrument));
        }

        return parseTags (object, path, out.tags)
            && parseOptionalYanked (object, path, out.yanked);
    }

    bool parse (const Json& root, CatalogPayload& out)
    {
        if (! root.is_object())
            return fail ("root", "expected an object");
        if (! validateKeys (root, "root",
                            { "catalog_id", "schema_version", "catalog_revision",
                              "generated_at", "packs" }))
            return false;

        if (! readString (root, "catalog_id", "root", out.catalogId,
                          kMaxCatalogIdLength)
            || ! validateStableId (out.catalogId, "root.catalog_id",
                                   kMaxCatalogIdLength))
            return false;

        std::uint64_t schemaVersion = 0;
        if (! readPositiveInteger (root, "schema_version", "root",
                                   std::numeric_limits<std::uint32_t>::max(),
                                   schemaVersion))
            return false;
        if (schemaVersion != kSchemaVersion)
            return fail ("root.schema_version", "unsupported catalog schema version");
        out.schemaVersion = static_cast<std::uint32_t> (schemaVersion);

        if (! readPositiveInteger (root, "catalog_revision", "root",
                                   std::numeric_limits<std::uint64_t>::max(),
                                   out.catalogRevision)
            || ! readString (root, "generated_at", "root", out.generatedAt,
                             kMaxTimestampLength))
            return false;

        const auto* packs = required (root, "packs", "root");
        if (packs == nullptr)
            return false;
        if (! packs->is_array())
            return fail ("root.packs", "expected an array");
        if (packs->empty())
            return fail ("root.packs", "must not be empty");
        if (packs->size() > kMaxPacks)
            return fail ("root.packs", "contains too many packs");

        std::unordered_set<std::string> packReleaseIds;
        out.packs.reserve (packs->size());
        for (std::size_t i = 0; i < packs->size(); ++i)
        {
            const auto itemPath = "root.packs[" + std::to_string (i) + "]";
            CatalogPack pack;
            if (! parsePack ((*packs)[i], itemPath, pack))
                return false;
            const auto identity = pack.id + '\0' + pack.releaseId;
            if (! packReleaseIds.insert (identity).second)
                return fail (itemPath, "duplicates a pack and release identity");
            out.packs.push_back (std::move (pack));
        }
        return true;
    }
};
} // namespace

CatalogParseResult parseCatalogPayload (std::string_view source) noexcept
{
    try
    {
        if (source.size() > kMaxPayloadBytes)
            return { std::nullopt, "root: payload exceeds the 8 MiB limit" };

        std::vector<std::unordered_set<std::string>> objectKeys;
        bool duplicateKeyFound = false;
        std::string duplicateKey;
        const auto callback = [&objectKeys, &duplicateKeyFound, &duplicateKey] (
            int, Json::parse_event_t event, Json& parsed)
        {
            if (event == Json::parse_event_t::object_start)
            {
                objectKeys.emplace_back();
            }
            else if (event == Json::parse_event_t::key && ! objectKeys.empty())
            {
                const auto& key = parsed.get_ref<const std::string&>();
                if (! objectKeys.back().insert (key).second && ! duplicateKeyFound)
                {
                    duplicateKeyFound = true;
                    duplicateKey = key;
                }
            }
            else if (event == Json::parse_event_t::object_end && ! objectKeys.empty())
            {
                objectKeys.pop_back();
            }
            return true;
        };

        const auto root = Json::parse (source.begin(), source.end(), callback, false);
        if (root.is_discarded())
            return { std::nullopt, "root: malformed JSON" };
        if (duplicateKeyFound)
            return { std::nullopt, "root: duplicate object key '" + duplicateKey + "'" };

        Parser parser;
        CatalogPayload catalog;
        if (! parser.parse (root, catalog))
            return { std::nullopt, std::move (parser.error) };
        return { std::move (catalog), {} };
    }
    catch (...)
    {
        return { std::nullopt, "root: catalog parsing failed" };
    }
}
} // namespace duskstudio::sfz

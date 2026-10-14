#include "SfzProvider.h"
#include "SfzCatalogJson.h"
#include "SfzPathPolicy.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <set>
#include <utility>

namespace duskstudio::sfz
{
namespace
{
using Json = dusk::json::Json;
constexpr std::size_t kMaxCandidates = 4096;
constexpr std::size_t kMaxLicenseBytes = 200000;

std::string lower (std::string value)
{
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char> (c + 'a' - 'A');
    return value;
}

std::string string (const Json& object, const char* key, std::size_t limit = 8192)
{
    const auto found = object.find (key);
    if (found == object.end() || ! found->is_string()) return {};
    const auto& value = found->get_ref<const std::string&>();
    return value.size() <= limit && value.find ('\0') == std::string::npos ? value : std::string{};
}

std::uint64_t integer (const Json& object, const char* key)
{
    const auto found = object.find (key);
    return found != object.end() && found->is_number_unsigned() ? found->get<std::uint64_t>() : 0;
}

bool decimalId (std::string_view value)
{
    return ! value.empty() && value.size() <= 20 && value.front() != '0'
        && std::all_of (value.begin(), value.end(), [] (char c) { return c >= '0' && c <= '9'; });
}

bool gitSha (std::string_view value)
{
    return value.size() == 40 && std::all_of (value.begin(), value.end(), [] (char c)
    { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool repositoryName (std::string_view value)
{
    return ! value.empty() && value.size() <= 100 && value != "." && value != ".."
        && std::all_of (value.begin(), value.end(), [] (char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.';
    });
}

bool https (std::string_view value)
{
    if (value.substr (0, 8) != "https://" || value.size() > 8192
        || value.find_first_of ("\r\n\t \\") != std::string_view::npos) return false;
    const auto authority = value.substr (8, value.find_first_of ("/?#", 8) - 8);
    return ! authority.empty() && authority.find ('@') == std::string_view::npos;
}

std::string encode (std::string_view value, bool retainSlash = false)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const auto byte : value)
    {
        const auto c = static_cast<unsigned char> (byte);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~' || (retainSlash && c == '/')) result += byte;
        else { result += '%'; result += digits[c >> 4]; result += digits[c & 15]; }
    }
    return result;
}

std::string sha256 (std::string_view value)
{
    if (sodium_init() < 0) return {};
    std::array<unsigned char, crypto_hash_sha256_BYTES> bytes {};
    crypto_hash_sha256 (bytes.data(), reinterpret_cast<const unsigned char*> (value.data()), value.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
    sodium_bin2hex (hex.data(), hex.size(), bytes.data(), bytes.size());
    return hex.data();
}

struct CcLicense
{
    std::string id, slug, version, url, constraints;
};

std::optional<CcLicense> ccLicense (std::string identifier)
{
    identifier = lower (std::move (identifier));
    if (identifier == "cc0-1.0")
        return CcLicense { "CC0-1.0", "zero", "1.0", "https://creativecommons.org/publicdomain/zero/1.0/",
                           "CC0 public-domain dedication; no attribution condition." };
    constexpr const char* types[] = { "by", "by-sa", "by-nd", "by-nc", "by-nc-sa", "by-nc-nd" };
    constexpr const char* versions[] = { "1.0", "2.0", "2.5", "3.0", "4.0" };
    for (const auto* type : types)
        for (const auto* version : versions)
            if (identifier == std::string ("cc-") + type + "-" + version)
            {
                std::string id = identifier;
                for (auto& c : id) if (c >= 'a' && c <= 'z') c = static_cast<char> (c + 'A' - 'a');
                std::string constraints = "Attribution required.";
                if (std::string_view (type).find ("nc") != std::string_view::npos) constraints += " NonCommercial restriction.";
                if (std::string_view (type).find ("sa") != std::string_view::npos) constraints += " ShareAlike requirement.";
                if (std::string_view (type).find ("nd") != std::string_view::npos) constraints += " NoDerivatives restriction.";
                return CcLicense { id, type, version,
                    std::string ("https://creativecommons.org/licenses/") + type + "/" + version + "/", constraints };
            }
    return {};
}

std::optional<CcLicense> artifactLicense (const std::string& shortName)
{
    if (shortName == "cc-zero") return ccLicense ("CC0-1.0");
    auto type = shortName;
    std::string version = "4.0";
    if (type.size() > 2 && type.substr (type.size() - 2) == "-3")
    { type.resize (type.size() - 2); version = "3.0"; }
    return ccLicense ("cc-" + type + "-" + version);
}

bool formalLicenseTitle (const std::string& text, const CcLicense& license)
{
    auto title = lower (text);
    std::replace (title.begin(), title.end(), '-', ' ');
    if (title.find ("creative commons") == std::string::npos) return false;
    if (license.slug == "zero")
        return title.find ("cc0 1.0") != std::string::npos || title.find ("zero 1.0") != std::string::npos;
    std::string name = "attribution";
    if (license.slug.find ("nc") != std::string::npos) name += " noncommercial";
    if (license.slug.find ("sa") != std::string::npos) name += " sharealike";
    if (license.slug.find ("nd") != std::string::npos)
        return title.find (name + " noderivatives " + license.version) != std::string::npos
            || title.find (name + " noderivs " + license.version) != std::string::npos;
    return title.find (name + " " + license.version) != std::string::npos;
}

std::optional<CcLicense> licenseText (const std::string& text, const std::string& hint)
{
    const auto content = lower (text);
    const auto containsUrl = [&] (const std::string& marker)
    {
        auto offset = content.find (marker);
        while (offset != std::string::npos)
        {
            const auto end = offset + marker.size();
            if (end == content.size() || std::string_view ("/ \r\n\t\"')]>?#").find (content[end]) != std::string_view::npos) return true;
            offset = content.find (marker, offset + 1);
        }
        return false;
    };
    std::set<std::string> found;
    constexpr const char* types[] = { "by", "by-sa", "by-nd", "by-nc", "by-nc-sa", "by-nc-nd" };
    constexpr const char* versions[] = { "1.0", "2.0", "2.5", "3.0", "4.0" };
    if (containsUrl ("//creativecommons.org/publicdomain/zero/1.0")
        || content.find ("cc0 1.0 universal") != std::string::npos) found.insert ("CC0-1.0");
    for (const auto* type : types)
        for (const auto* version : versions)
        {
            const auto license = ccLicense (std::string ("cc-") + type + "-" + version);
            const auto marker = std::string ("//creativecommons.org/licenses/") + type + "/" + version;
            if (containsUrl (marker) || formalLicenseTitle (text, *license)) found.insert (license->id);
        }
    if (found.size() != 1 || content.find ("mit license") != std::string::npos
        || content.find ("gnu general public license") != std::string::npos) return {};
    const auto actual = ccLicense (*found.begin());
    const auto claimed = ccLicense (hint);
    if (claimed && claimed->id != actual->id) return {};
    return actual;
}

std::string responseHeader (const HttpResponse& response, const char* name, std::size_t limit = 128)
{
    const auto found = response.headers.find (name);
    return found != response.headers.end() && found->second.size() <= limit ? found->second : std::string{};
}

struct ApiResult : OperationResult
{
    Json json;
    HttpResponse response;
    ProviderRateLimit rateLimit;
};

ApiResult api (HttpTransport& transport, const std::string& url, std::uint64_t maxBytes,
               const OperationControl& control)
{
    HttpRequest request;
    request.url = url;
    request.headers["accept"] = "application/json";
    if (url.substr (0, 23) == "https://api.github.com/")
    {
        request.headers["accept"] = "application/vnd.github+json";
        request.headers["x-github-api-version"] = "2022-11-28";
    }
    const auto response = fetchHttpText (transport, request, maxBytes, control);
    ApiResult result;
    result.status = response.status;
    result.error = response.error;
    result.response = response.response;
    result.rateLimit = { responseHeader (response.response, "x-ratelimit-remaining"),
                         responseHeader (response.response, "x-ratelimit-reset"),
                         responseHeader (response.response, "retry-after") };
    if (! response) return result;
    const auto contentType = lower (responseHeader (response.response, "content-type"));
    const auto first = response.body.find_first_not_of (" \r\n\t");
    const bool html = contentType.find ("text/html") != std::string::npos
        || (first != std::string::npos && response.body[first] == '<');
    if (response.response.statusCode == 429
        || (response.response.statusCode == 403 && result.rateLimit.remaining == "0"))
        result.error = "Repository rate limit reached. Retry after the server's indicated wait; the source link remains available.";
    else if (html)
        result.error = "Repository returned a browser or anti-bot page. Open the source website; automated downloads are unavailable.";
    else if (response.response.statusCode == 403)
        result.error = "Repository denied API access. Try again later or open the source website.";
    else if (response.response.statusCode != 200)
        result.error = "Repository request failed (HTTP " + std::to_string (response.response.statusCode) + ").";
    if (! result.error.empty()) { result.status = OperationStatus::Failed; return result; }
    const auto parsed = detail::parseJsonRejectingDuplicateKeys (response.body, "Repository response");
    if (! parsed) { result.status = OperationStatus::Failed; result.error = parsed.error; return result; }
    result.json = *parsed.root;
    return result;
}

bool sfzPath (std::string_view path)
{
    return path.size() >= 4 && lower (std::string (path.substr (path.size() - 4))) == ".sfz";
}

bool declaresSfz (const Json& json)
{
    const auto found = json.find ("formats");
    if (found == json.end() || ! found->is_array() || found->size() > 100) return false;
    return std::any_of (found->begin(), found->end(), [] (const Json& format)
    { return format.is_string() && lower (format.get<std::string>()) == "sfz"; });
}

bool addInstrument (ProviderPack& pack, const std::string& path, std::set<std::string>& paths)
{
    std::string error;
    if (! validateRelativePath (path, error)) return false;
    const auto key = portablePathKey (path);
    if (! key || ! paths.insert (*key).second || pack.instruments.size() >= kMaxCandidates) return false;
    const auto slash = path.find_last_of ('/');
    pack.instruments.push_back ({ "sfz-" + sha256 (path).substr (0, 32),
        path.substr (slash == std::string::npos ? 0 : slash + 1), path, true });
    return true;
}

ProviderPackResult githubPack (HttpTransport& transport, const ProviderSummary& summary,
                               const OperationControl& control)
{
    ProviderPackResult result;
    const auto fail = [&] (const std::string& error)
    { result.status = OperationStatus::Failed; result.error = error; return result; };
    if (! repositoryName (summary.locator) || ! decimalId (summary.packId)) return fail ("Invalid SFZ Instruments repository locator");
    const auto base = "https://api.github.com/repos/sfzinstruments/" + summary.locator;
    const auto repository = api (transport, base, 1048576, control);
    result.rateLimit = repository.rateLimit;
    if (! repository) { result.status = repository.status; result.error = repository.error; return result; }
    if (std::to_string (integer (repository.json, "id")) != summary.packId
        || lower (string (dusk::json::child (repository.json, "owner"), "login", 100)) != "sfzinstruments")
        return fail ("Repository identity changed; refresh the source listing");
    const auto commit = api (transport, base + "/commits/HEAD", 1048576, control);
    result.rateLimit = commit.rateLimit;
    if (! commit) { result.status = commit.status; result.error = commit.error; return result; }
    const auto revision = string (commit.json, "sha", 40);
    const auto treeSha = string (dusk::json::child (dusk::json::child (commit.json, "commit"), "tree"), "sha", 40);
    if (! gitSha (revision) || ! gitSha (treeSha)) return fail ("Repository did not return a complete immutable Git revision");
    const auto tree = api (transport, base + "/git/trees/" + treeSha + "?recursive=1", 8 * 1048576, control);
    result.rateLimit = tree.rateLimit;
    if (! tree) { result.status = tree.status; result.error = tree.error; return result; }
    const auto entries = tree.json.find ("tree");
    const auto truncated = tree.json.find ("truncated");
    if (entries == tree.json.end() || ! entries->is_array() || entries->size() > 50000
        || truncated == tree.json.end() || ! truncated->is_boolean() || truncated->get<bool>())
        return fail ("Repository tree is incomplete or too large to resolve safely");
    ProviderPack pack;
    pack.sourceId = "sfzinstruments";
    pack.packId = summary.packId;
    pack.releaseId = revision;
    pack.displayName = summary.locator;
    pack.author = "SFZ Instruments";
    pack.sourceUrl = "https://github.com/sfzinstruments/" + summary.locator + "/tree/" + revision;
    pack.downloadUrl = "https://github.com/sfzinstruments/" + summary.locator + "/archive/" + revision + ".zip";
    pack.expectedRoot = summary.locator + "-" + revision;
    pack.versionPinned = true;
    std::set<std::string> paths;
    std::set<std::string> regularFiles;
    for (const auto& entry : *entries)
    {
        const auto mode = string (entry, "mode", 16);
        if (string (entry, "type", 16) != "blob" || (mode != "100644" && mode != "100755")) continue;
        const auto path = string (entry, "path", 512);
        regularFiles.insert (path);
        if (sfzPath (path) && ! addInstrument (pack, path, paths)) return fail ("Repository has unsafe or ambiguous SFZ paths");
    }
    if (pack.instruments.empty()) return fail ("Repository contains no SFZ files at the pinned revision");
    const auto license = api (transport, base + "/license?ref=" + revision, 400000, control);
    result.rateLimit = license.rateLimit;
    if (! license) { result.status = license.status; result.error = license.error; return result; }
    const auto path = string (license.json, "path", 512);
    const auto encoded = string (license.json, "content", 300000);
    const auto hint = string (dusk::json::child (license.json, "license"), "spdx_id", 100);
    std::string pathError;
    if (string (license.json, "encoding", 16) != "base64" || ! validateRelativePath (path, pathError)
        || regularFiles.count (path) == 0 || encoded.empty()) return fail ("Repository LICENSE file cannot be verified at the pinned revision");
    std::string text (kMaxLicenseBytes, '\0');
    std::size_t decoded = 0;
    if (sodium_init() < 0 || sodium_base642bin (reinterpret_cast<unsigned char*> (text.data()), text.size(),
        encoded.data(), encoded.size(), " \t\r\n", &decoded, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0)
        return fail ("Repository LICENSE content is invalid or too large");
    text.resize (decoded);
    if (text.find ('\0') != std::string::npos || ! portablePathKey (text)) return fail ("Repository LICENSE is not valid text");
    const auto recognized = licenseText (text, hint);
    if (! recognized) return fail ("Repository LICENSE does not establish one supported Creative Commons license");
    pack.license = { recognized->id, text, path, sha256 (text),
        "https://github.com/sfzinstruments/" + summary.locator + "/blob/" + revision + "/" + encode (path, true), recognized->constraints };
    std::sort (pack.instruments.begin(), pack.instruments.end(), [] (const auto& a, const auto& b)
    { return a.relativePath < b.relativePath; });
    result.pack = std::move (pack);
    result.status = OperationStatus::Complete;
    return result;
}

ProviderPackResult artifactPack (HttpTransport& transport, const ProviderSummary& summary,
                                 const OperationControl& control)
{
    ProviderPackResult result;
    const auto fail = [&] (const std::string& error)
    { result.status = OperationStatus::Failed; result.error = error; return result; };
    if (! decimalId (summary.packId) || summary.locator != summary.packId) return fail ("Invalid Musical Artifacts locator");
    const auto source = "https://musical-artifacts.com/artifacts/" + summary.packId;
    const auto detail = api (transport, source + ".json", 2 * 1048576, control);
    result.rateLimit = detail.rateLimit;
    if (! detail) { result.status = detail.status; result.error = detail.error; return result; }
    if (std::to_string (integer (detail.json, "id")) != summary.packId) return fail ("Musical Artifacts returned a different artifact");
    if (! declaresSfz (detail.json)) return fail ("Artifact does not declare SFZ content");
    const auto shortName = string (detail.json, "license", 100);
    const auto recognized = artifactLicense (shortName);
    if (! recognized) return fail ("Artifact does not declare a supported Creative Commons license");
    const auto licenses = api (transport, "https://musical-artifacts.com/licenses.json", 262144, control);
    result.rateLimit = licenses.rateLimit;
    if (! licenses) { result.status = licenses.status; result.error = licenses.error; return result; }
    if (! licenses.json.is_array() || licenses.json.size() > 256) return fail ("Musical Artifacts license definitions are invalid");
    std::string declaration;
    for (const auto& license : licenses.json)
        if (string (license, "short_name", 100) == shortName
            && string (license, "license_type", 100) == "cc")
        {
            const auto name = string (license, "name", 512);
            if (formalLicenseTitle (name, *recognized)) declaration = name;
        }
    if (declaration.empty()) return fail ("Artifact Creative Commons declaration cannot be verified from the provider's license definitions");
    const auto download = string (detail.json, "file");
    const auto prefix = source + "/";
    auto filename = lower (download.substr (0, download.find_first_of ("?#")));
    if (! https (download) || download.substr (0, prefix.size()) != prefix)
        return fail ("Artifact has no supported direct HTTPS download");
    if (filename.size() < 4 || filename.substr (filename.size() - 4) != ".zip")
        return fail ("Artifact archive format is unsupported. Choose a ZIP pack or download this item from its source website.");
    ProviderPack pack;
    pack.sourceId = "musical-artifacts";
    pack.packId = summary.packId;
    pack.displayName = string (detail.json, "name", 256);
    pack.author = string (detail.json, "author", 256);
    pack.sourceUrl = source;
    pack.downloadUrl = download;
    const auto advertisedHash = detail.json.find ("file_hash");
    if (advertisedHash != detail.json.end() && ! advertisedHash->is_null()
        && (! advertisedHash->is_string()
            || (! advertisedHash->get_ref<const std::string&>().empty()
                && ! isSha256 (lower (advertisedHash->get_ref<const std::string&>())))))
        return fail ("Artifact SHA-256 metadata is invalid");
    pack.expectedSha256 = lower (string (detail.json, "file_hash", 64));
    if (! pack.expectedSha256.empty() && ! isSha256 (pack.expectedSha256)) return fail ("Artifact SHA-256 metadata is invalid");
    const auto fileId = integer (detail.json, "file_id");
    pack.versionPinned = ! pack.expectedSha256.empty();
    pack.releaseId = ! pack.expectedSha256.empty() ? pack.expectedSha256
        : fileId != 0 ? "file-" + std::to_string (fileId)
        : "metadata-" + sha256 (download + "\n" + shortName);
    pack.advertisedBytes = integer (detail.json, "file_size");
    const auto text = recognized->id + "\n" + declaration + "\nDeclared for " + pack.displayName
        + " by " + pack.author + "\nSource: " + source + ".json\nTerms: " + recognized->url + "\n";
    pack.license = { recognized->id, text, {}, sha256 (text), source, recognized->constraints };
    std::set<std::string> paths;
    const auto files = detail.json.find ("file_list");
    if (files != detail.json.end() && ! files->is_null())
    {
        if (! files->is_array() || files->size() > 50000) return fail ("Artifact file listing is invalid or too large");
        for (const auto& file : *files)
        {
            if (! file.is_string()) return fail ("Artifact file listing contains invalid paths");
            auto path = file.get<std::string>();
            if (path.substr (0, 2) == "./") path.erase (0, 2);
            if (sfzPath (path) && ! addInstrument (pack, path, paths)) return fail ("Artifact has unsafe or ambiguous SFZ paths");
        }
    }
    if (pack.displayName.empty()) return fail ("Artifact name is missing");
    result.pack = std::move (pack);
    result.status = OperationStatus::Complete;
    return result;
}
} // namespace

std::vector<ProviderSource> providerSources()
{
    return { { "sfzinstruments", "SFZ Instruments", "https://sfzinstruments.github.io/" },
             { "musical-artifacts", "Musical Artifacts", "https://musical-artifacts.com/" } };
}

ProviderPageResult browseProvider (HttpTransport& transport, const std::string& sourceId,
                                   unsigned page, const std::string& query, const OperationControl& control)
{
    ProviderPageResult result;
    if (page == 0 || page > 200 || query.size() > 256
        || (sourceId != "sfzinstruments" && sourceId != "musical-artifacts"))
    { result.error = "Invalid repository browse request"; return result; }
    const bool github = sourceId == "sfzinstruments";
    const auto url = github
        ? "https://api.github.com/orgs/sfzinstruments/repos?type=public&sort=full_name&per_page=50&page=" + std::to_string (page)
        : "https://musical-artifacts.com/artifacts.json?formats=sfz&page=" + std::to_string (page) + "&q=" + encode (query);
    const auto response = api (transport, url, 2 * 1048576, control);
    result.rateLimit = response.rateLimit;
    if (! response) { result.status = response.status; result.error = response.error; return result; }
    if (! response.json.is_array() || response.json.size() > 256)
    { result.error = "Repository listing is invalid or too large"; return result; }
    std::set<std::string> ids;
    for (const auto& entry : response.json)
    {
        const auto id = integer (entry, "id");
        if (id == 0) continue;
        ProviderSummary summary;
        summary.sourceId = sourceId;
        summary.packId = std::to_string (id);
        summary.name = string (entry, "name", 256);
        summary.description = string (entry, "description", 4096);
        if (github)
        {
            summary.locator = summary.name;
            summary.author = string (dusk::json::child (entry, "owner"), "login", 100);
            const auto disabled = entry.find ("disabled"), privateRepo = entry.find ("private");
            if (! repositoryName (summary.locator) || lower (summary.author) != "sfzinstruments"
                || (disabled != entry.end() && disabled->is_boolean() && disabled->get<bool>())
                || (privateRepo != entry.end() && privateRepo->is_boolean() && privateRepo->get<bool>())) continue;
            summary.sourceUrl = "https://github.com/sfzinstruments/" + summary.locator;
            summary.licenseHint = string (dusk::json::child (entry, "license"), "spdx_id", 100);
            if (! summary.licenseHint.empty() && summary.licenseHint != "NOASSERTION"
                && ! ccLicense (summary.licenseHint)) continue;
            if (! query.empty() && lower (summary.name + " " + summary.description).find (lower (query)) == std::string::npos) continue;
        }
        else
        {
            summary.locator = summary.packId;
            summary.author = string (entry, "author", 256);
            const auto license = artifactLicense (string (entry, "license", 100));
            if (! license || ! declaresSfz (entry)) continue;
            summary.licenseHint = license->id;
            summary.sourceUrl = "https://musical-artifacts.com/artifacts/" + summary.packId;
        }
        if (! summary.name.empty() && ids.insert (summary.packId).second) result.items.push_back (std::move (summary));
    }
    const auto link = responseHeader (response.response, "link", 8192);
    const auto total = responseHeader (response.response, "x-total-pages");
    unsigned totalPages = 0;
    const auto parsed = std::from_chars (total.data(), total.data() + total.size(), totalPages);
    if (page < 200 && (link.find ("rel=\"next\"") != std::string::npos
        || (! total.empty() && parsed.ec == std::errc{} && parsed.ptr == total.data() + total.size() && page < totalPages)
        || (github && response.json.size() == 50))) result.nextPage = page + 1;
    result.status = OperationStatus::Complete;
    return result;
}

ProviderPackResult resolveProviderPack (HttpTransport& transport, const ProviderSummary& summary,
                                       const OperationControl& control)
{
    if (sodium_init() < 0)
    {
        ProviderPackResult result;
        result.error = "Cannot initialize SFZ descriptor hashing";
        return result;
    }
    if (summary.sourceId == "sfzinstruments") return githubPack (transport, summary, control);
    if (summary.sourceId == "musical-artifacts") return artifactPack (transport, summary, control);
    ProviderPackResult result;
    result.error = "Unknown SFZ repository";
    return result;
}

std::string providerPackToJson (const ProviderPack& pack)
{
    Json instruments = Json::array();
    for (const auto& instrument : pack.instruments)
        instruments.push_back ({ { "id", instrument.id }, { "name", instrument.name },
                                 { "path", instrument.relativePath }, { "candidate_only", instrument.candidateOnly } });
    return Json {
        { "schema", 1 }, { "source_id", pack.sourceId }, { "pack_id", pack.packId }, { "release_id", pack.releaseId },
        { "display_name", pack.displayName }, { "author", pack.author }, { "source_url", pack.sourceUrl },
        { "download_url", pack.downloadUrl }, { "expected_root", pack.expectedRoot },
        { "expected_sha256", pack.expectedSha256 }, { "advertised_bytes", pack.advertisedBytes },
        { "version_pinned", pack.versionPinned },
        { "license", { { "id", pack.license.id }, { "text", pack.license.text }, { "path", pack.license.path },
                       { "sha256", pack.license.sha256 }, { "source_url", pack.license.sourceUrl },
                       { "constraints", pack.license.constraints } } }, { "instruments", instruments }
    }.dump();
}

ProviderPackResult providerPackFromJson (std::string_view source)
{
    ProviderPackResult result;
    const auto fail = [&] (const std::string& error) { result.error = error; return result; };
    if (source.size() > 2 * 1048576) return fail ("Cached SFZ descriptor exceeds the size limit");
    const auto parsed = detail::parseJsonRejectingDuplicateKeys (source, "Cached SFZ descriptor");
    if (! parsed) return fail (parsed.error);
    const auto& json = *parsed.root;
    const auto schema = json.find ("schema");
    if (! json.is_object() || schema == json.end() || ! schema->is_number_integer() || *schema != 1)
        return fail ("Unsupported cached SFZ descriptor schema");
    const std::pair<const char*, std::size_t> fields[] = {
        { "source_id", 100 }, { "pack_id", 20 }, { "release_id", 100 }, { "display_name", 256 },
        { "author", 256 }, { "source_url", 8192 }, { "download_url", 8192 },
        { "expected_root", 255 }, { "expected_sha256", 64 }
    };
    for (const auto& item : fields)
    {
        const auto field = json.find (item.first);
        if (field == json.end() || ! field->is_string()
            || field->get_ref<const std::string&>().size() > item.second
            || field->get_ref<const std::string&>().find ('\0') != std::string::npos)
            return fail ("Cached SFZ descriptor field types or sizes are invalid");
    }
    const auto pinned = json.find ("version_pinned"), advertised = json.find ("advertised_bytes");
    if (pinned == json.end() || ! pinned->is_boolean() || advertised == json.end() || ! advertised->is_number_unsigned())
        return fail ("Cached SFZ version and size types are invalid");
    ProviderPack pack;
    pack.sourceId = string (json, "source_id", 100);
    pack.packId = string (json, "pack_id", 20);
    pack.releaseId = string (json, "release_id", 100);
    pack.displayName = string (json, "display_name", 256);
    pack.author = string (json, "author", 256);
    pack.sourceUrl = string (json, "source_url");
    pack.downloadUrl = string (json, "download_url");
    pack.expectedRoot = string (json, "expected_root", 255);
    pack.expectedSha256 = string (json, "expected_sha256", 64);
    pack.advertisedBytes = integer (json, "advertised_bytes");
    pack.versionPinned = dusk::json::getBool (json, "version_pinned", false);
    const auto& license = dusk::json::child (json, "license");
    const std::pair<const char*, std::size_t> licenseFields[] = {
        { "id", 100 }, { "text", kMaxLicenseBytes }, { "path", 512 }, { "sha256", 64 },
        { "source_url", 8192 }, { "constraints", 1024 }
    };
    for (const auto& item : licenseFields)
    {
        const auto field = license.find (item.first);
        if (field == license.end() || ! field->is_string()
            || field->get_ref<const std::string&>().size() > item.second
            || field->get_ref<const std::string&>().find ('\0') != std::string::npos)
            return fail ("Cached SFZ license field types or sizes are invalid");
    }
    pack.license = { string (license, "id", 100), string (license, "text", kMaxLicenseBytes),
        string (license, "path", 512), string (license, "sha256", 64), string (license, "source_url"),
        string (license, "constraints", 1024) };
    const auto recognized = ccLicense (pack.license.id);
    std::string pathError;
    if (! decimalId (pack.packId) || pack.displayName.empty() || ! https (pack.sourceUrl) || ! https (pack.downloadUrl)
        || ! https (pack.license.sourceUrl) || ! recognized || pack.license.text.empty()
        || ! isSha256 (pack.license.sha256) || sha256 (pack.license.text) != pack.license.sha256
        || (! pack.expectedSha256.empty() && ! isSha256 (pack.expectedSha256))
        || (! pack.expectedRoot.empty() && (pack.expectedRoot.find ('/') != std::string::npos
                                            || ! validateRelativePath (pack.expectedRoot, pathError)))
        || (! pack.license.path.empty() && ! validateRelativePath (pack.license.path, pathError)))
        return fail ("Cached SFZ descriptor has invalid identity, license, URL, or paths");
    if (pack.sourceId == "sfzinstruments")
    {
        const std::string prefix = "https://github.com/sfzinstruments/";
        const auto suffix = pack.downloadUrl.substr (std::min (pack.downloadUrl.size(), prefix.size()));
        const auto repository = suffix.substr (0, suffix.find ('/'));
        if (! gitSha (pack.releaseId) || ! pack.versionPinned || ! pack.expectedSha256.empty()
            || ! repositoryName (repository) || pack.license.path.empty()
            || pack.downloadUrl != prefix + repository + "/archive/" + pack.releaseId + ".zip"
            || pack.expectedRoot != repository + "-" + pack.releaseId
            || pack.sourceUrl != prefix + repository + "/tree/" + pack.releaseId
            || pack.license.sourceUrl != prefix + repository + "/blob/" + pack.releaseId + "/" + encode (pack.license.path, true)
            || ! licenseText (pack.license.text, pack.license.id)) return fail ("Cached GitHub SFZ version is not pinned and licensed");
    }
    else if (pack.sourceId == "musical-artifacts")
    {
        const auto prefix = "https://musical-artifacts.com/artifacts/" + pack.packId;
        const auto archiveUrl = lower (pack.downloadUrl.substr (0, pack.downloadUrl.find_first_of ("?#")));
        if (pack.downloadUrl.substr (0, prefix.size() + 1) != prefix + "/"
            || pack.sourceUrl != prefix || pack.license.sourceUrl != prefix
            || ! pack.license.path.empty() || ! pack.expectedRoot.empty() || pack.releaseId.empty()
            || pack.license.text.find (recognized->id) == std::string::npos
            || pack.license.text.find (recognized->url) == std::string::npos
            || archiveUrl.size() < 4 || archiveUrl.substr (archiveUrl.size() - 4) != ".zip")
            return fail ("Cached Musical Artifacts version is invalid");
        if ((! pack.expectedSha256.empty() && (pack.releaseId != pack.expectedSha256 || ! pack.versionPinned))
            || (pack.expectedSha256.empty() && pack.releaseId.substr (0, 5) == "file-"
                && (! decimalId (pack.releaseId.substr (5)) || pack.versionPinned))
            || (pack.expectedSha256.empty() && pack.releaseId.substr (0, 9) == "metadata-"
                && (! isSha256 (pack.releaseId.substr (9)) || pack.versionPinned))
            || (pack.expectedSha256.empty() && pack.releaseId.substr (0, 5) != "file-"
                && pack.releaseId.substr (0, 9) != "metadata-")) return fail ("Cached artifact version identity is invalid");
    }
    else return fail ("Cached SFZ descriptor names an unknown repository");
    pack.license.id = recognized->id;
    pack.license.constraints = recognized->constraints;
    const auto entries = json.find ("instruments");
    if (entries == json.end() || ! entries->is_array() || entries->size() > kMaxCandidates)
        return fail ("Cached SFZ instrument list is invalid");
    std::set<std::string> paths, ids;
    for (const auto& entry : *entries)
    {
        const auto id = string (entry, "id", 100), name = string (entry, "name", 256), path = string (entry, "path", 512);
        if (id.empty() || id == "." || id == ".." || id.find_first_not_of ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::string::npos
            || name.empty() || ! sfzPath (path) || ! addInstrument (pack, path, paths)
            || ! ids.insert (id).second) return fail ("Cached SFZ instrument identity or path is invalid");
        auto& instrument = pack.instruments.back();
        instrument.id = id;
        instrument.name = name;
        const auto candidate = entry.find ("candidate_only");
        if (candidate == entry.end() || ! candidate->is_boolean()) return fail ("Cached SFZ candidate type is invalid");
        instrument.candidateOnly = candidate->get<bool>();
    }
    result.pack = std::move (pack);
    result.status = OperationStatus::Complete;
    return result;
}
} // namespace duskstudio::sfz

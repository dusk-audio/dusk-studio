#include "SfzProvider.h"
#include "SfzCatalogJson.h"
#include "SfzInstrumentCategory.h"
#include "SfzPathPolicy.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <set>
#include <sstream>
#include <utility>

namespace duskstudio::sfz
{
namespace
{
using Json = dusk::json::Json;
constexpr std::size_t kMaxCandidates = 4096;

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

const char* githubOwner (std::string_view source)
{
    if (source == "sfzinstruments") return "sfzinstruments";
    if (source == "freepats") return "freepats";
    if (source == "versilian") return "sgossner";
    return nullptr;
}

bool githubRepositoryAllowed (std::string_view source, std::string_view repository)
{
    return repositoryName (repository)
        && (source != "versilian" || repository == "VCSL" || repository == "VSCO-2-CE");
}

bool https (std::string_view value)
{
    if (value.substr (0, 8) != "https://" || value.size() > 8192
        || value.find_first_of ("\r\n\t \\") != std::string_view::npos) return false;
    const auto authority = value.substr (8, value.find_first_of ("/?#", 8) - 8);
    return ! authority.empty() && authority.find ('@') == std::string_view::npos;
}

void presentationMetadata (const Json& object, ProviderPresentation& presentation, std::string_view sourceId)
{
    presentation.categoryHint = string (object, "description", 2048);
    for (const auto* field : { "tags", "topics" })
    {
        const auto list = object.find (field);
        if (list == object.end() || ! list->is_array() || list->size() > 64) continue;
        for (const auto& tag : *list)
            if (tag.is_string() && tag.get_ref<const std::string&>().size() <= 64 && presentation.categoryHint.size() < 4096)
                presentation.categoryHint += " " + tag.get<std::string>();
    }
    for (const auto* field : { "image_url", "thumbnail_url", "image" })
    {
        const auto url = string (object, field, 2048);
        if (httpsUrlOnHosts (url, artworkHostsForSource (sourceId))) { presentation.artworkUrl = url; break; }
    }
}

std::optional<std::uint64_t> popularity (const Json& object, const char* key)
{
    const auto value = object.find (key);
    if (value == object.end()) return {};
    if (value->is_number_unsigned()) return value->get<std::uint64_t>();
    if (value->is_number_integer() && value->get<std::int64_t>() >= 0)
        return static_cast<std::uint64_t> (value->get<std::int64_t>());
    return {};
}

void artifactDemos (const Json& object, ProviderPresentation& presentation)
{
    const auto links = object.find ("more_info");
    if (links == object.end() || ! links->is_array() || links->size() > 64) return;
    std::set<std::string> seen;
    for (const auto& link : *links)
    {
        if (! link.is_string()) continue;
        const auto url = link.get<std::string>();
        if (url.size() > 2048 || url.find ('\0') != std::string::npos || ! https (url)) continue;
        if (std::any_of (url.begin(), url.end(), [] (unsigned char c) { return c <= 32 || c == 127; })) continue;
        const auto start = url.find_first_of ("/?#", 8);
        if (start == std::string::npos || url[start] != '/') continue;
        const auto host = lower (url.substr (8, start - 8));
        const auto path = url.substr (start, url.find_first_of ("?#", start) - start);
        std::string label;
        if ((host == "youtube.com" || host == "www.youtube.com" || host == "m.youtube.com")
            && ((path == "/watch" && url.find ("v=", start) != std::string::npos)
                || path.compare (0, 7, "/embed/") == 0 || path.compare (0, 8, "/shorts/") == 0)) label = "YouTube demo";
        else if (host == "youtu.be" && path.size() > 1) label = "YouTube demo";
        else if ((host == "vimeo.com" || host == "www.vimeo.com") && decimalId (std::string_view (path).substr (1))) label = "Vimeo demo";
        else if ((host == "soundcloud.com" || host == "www.soundcloud.com") && path.size() > 3
            && path.find ('/', 1) != std::string::npos && path.back() != '/') label = "SoundCloud demo";
        else
        {
            const auto extension = path.find_last_of ('.');
            const auto suffix = extension == std::string::npos ? std::string {} : lower (path.substr (extension));
            if (suffix == ".mp3" || suffix == ".wav" || suffix == ".flac" || suffix == ".ogg"
                || suffix == ".m4a" || suffix == ".opus") label = "Audio demo";
        }
        if (! label.empty() && seen.insert (url).second) presentation.demos.push_back ({ label, url });
        if (presentation.demos.size() == 4) break;
    }
}

std::string scalar (std::string value)
{
    const auto first = value.find_first_not_of (' ');
    if (first == std::string::npos) return {};
    value.erase (0, first);
    if (value.empty() || value.size() > 2048 || value.find_first_of ("\\\t\r\n") != std::string::npos) return {};
    if (value.front() == '\'' || value.front() == '"')
    {
        const auto end = value.find (value.front(), 1);
        if (end == std::string::npos) return {};
        const auto tail = value.find_first_not_of (' ', end + 1);
        if (tail != std::string::npos && value[tail] != '#') return {};
        return value.substr (1, end - 1);
    }
    const auto comment = value.find (" #");
    if (comment != std::string::npos) value.resize (comment);
    while (! value.empty() && value.back() == ' ') value.pop_back();
    if (value.find_first_of ("[]{}&*!|>'\"") != std::string::npos) return {};
    return value;
}

std::string catalogDescription (std::string value, bool block)
{
    const auto first = value.find_first_not_of (' ');
    if (first == std::string::npos) return {};
    value.erase (0, first);
    while (! value.empty() && value.back() == ' ') value.pop_back();
    if (value.size() > 2048 || value.find_first_of ("\\\t\r\n") != std::string::npos
        || value.find ('\0') != std::string::npos) return {};
    if (block) return value;
    if (value.front() == '\'' || value.front() == '"')
    {
        std::string result;
        const auto quote = value.front();
        for (std::size_t i = 1; i < value.size(); ++i)
        {
            if (value[i] != quote) { result += value[i]; continue; }
            if (quote == '\'' && i + 1 < value.size() && value[i + 1] == quote)
            { result += quote; ++i; continue; }
            const auto tail = value.find_first_not_of (' ', i + 1);
            return tail == std::string::npos || value[tail] == '#' ? result : std::string {};
        }
        return {};
    }
    if (value.find_first_of ("[]{}") != std::string::npos || value.find (": ") != std::string::npos
        || std::string_view ("&*!|>").find (value.front()) != std::string_view::npos) return {};
    const auto comment = value.find (" #");
    if (comment != std::string::npos) value.resize (comment);
    while (! value.empty() && value.back() == ' ') value.pop_back();
    return value;
}

struct CatalogMetadata
{
    std::string page, description, license;
    bool licenseUnreadable = false;
};

struct CatalogLookup
{
    std::optional<CatalogMetadata> declared, described;
};

// Catalog entries that link an author's site instead of the repository, paired after a manual review.
constexpr std::pair<std::string_view, std::string_view> kCatalogLinks[] { { "smdrums", "drums/sm_drums" } };

void appendMetadataText (std::string& text, const std::string& extra)
{
    if (extra.empty() || text.find (extra) != std::string::npos
        || text.size() + extra.size() + 1 > 8192) return;
    if (! text.empty()) text += ' ';
    text += extra;
}

CatalogLookup catalogLookup (std::string_view text, const std::string& repository)
{
    // Project explicit repository URLs and simple page slugs, without interpreting general YAML.
    struct Entry
    {
        bool active = false, invalid = false, downloads = false, hasPage = false, hasUrl = false;
        bool hasDescription = false, continuingDescription = false, descriptionBlock = false, descriptionInvalid = false;
        bool hasLicense = false, licenseUnreadable = false, github = false;
        std::string category, page, name, description, license;
        std::set<std::string> repositories, references;
    } entry;
    CatalogLookup result;
    if (text.empty()) return result;
    const auto key = lower (repository);
    std::string category;
    bool categoryPage = false, ambiguous = false;
    std::vector<CatalogMetadata> described, admitted;
    const auto pageSegment = [] (const std::string& value)
    { return repositoryName (value) && value.find ('.') == std::string::npos; };
    const auto finish = [&]
    {
        const auto slug = [] (std::string value)
        {
            for (auto& c : value)
            {
                if (c >= 'A' && c <= 'Z') c = static_cast<char> (c + 'a' - 'A');
                if (c == ' ' || c == '-' || c == '.') c = '_';
            }
            return value;
        };
        const auto shortRepository = repository.substr (repository.find ('.') == std::string::npos
            ? 0 : repository.find ('.') + 1);
        const bool nameMatch = repository.find ('.') != std::string::npos && entry.repositories.empty()
            && ! entry.name.empty() && slug (entry.name) == slug (shortRepository);
        const bool repositoryMatch = entry.repositories.size() == 1 && *entry.repositories.begin() == key;
        if ((nameMatch || repositoryMatch) && ! entry.hasPage) entry.page = slug (entry.name);
        const bool valid = entry.active && ! entry.invalid && pageSegment (entry.category) && pageSegment (entry.page);
        CatalogMetadata metadata { entry.category + "/" + entry.page,
            entry.descriptionInvalid ? std::string {} : catalogDescription (entry.description, entry.descriptionBlock),
            entry.license, entry.licenseUnreadable };
        const bool linked = entry.hasPage && ! entry.github && std::any_of (std::begin (kCatalogLinks), std::end (kCatalogLinks),
            [&] (const auto& link) { return link.first == key && link.second == metadata.page; });
        if (valid && (nameMatch || repositoryMatch)) described.push_back (metadata);
        if (valid && (repositoryMatch || linked)) admitted.push_back (std::move (metadata));
        else if (entry.active && (nameMatch || repositoryMatch || linked || entry.references.count (key) != 0))
            ambiguous = true;
    };
    const auto printable = [] (const std::string& value)
    { return std::all_of (value.begin(), value.end(), [] (char c) { return c >= 32 && c < 127; }); };
    std::istringstream input { std::string (text) };
    std::string line;
    std::size_t lines = 0, entries = 0;
    while (std::getline (input, line))
    {
        if (++lines > 8192 || line.size() > 2048 || line.find ('\t') != std::string::npos) return result;
        if (! line.empty() && line.back() == '\r') line.pop_back();
        const auto indent = line.find_first_not_of (' ');
        if (indent == std::string::npos || line[indent] == '#') continue;
        const auto content = line.substr (indent);
        if (entry.active && ! (indent <= 2 && content.compare (0, 7, "- name:") == 0))
        {
            const auto lowered = lower (content);
            constexpr std::string_view organization = "github.com/sfzinstruments/";
            if (lowered.find ("github.com/") != std::string::npos) entry.github = true;
            for (auto at = lowered.find (organization); at != std::string::npos; at = lowered.find (organization, at + 1))
            {
                const auto from = at + organization.size();
                const auto end = lowered.find_first_not_of ("abcdefghijklmnopqrstuvwxyz0123456789-_.", from);
                auto name = lowered.substr (from, end == std::string::npos ? end : end - from);
                for (;;)
                {
                    if (! name.empty() && name.back() == '.') name.pop_back();
                    else if (name.size() > 4 && name.compare (name.size() - 4, 4, ".git") == 0) name.resize (name.size() - 4);
                    else break;
                }
                entry.references.insert (name);
            }
        }
        if (entry.continuingDescription && indent >= 6)
        {
            if (content.size() + entry.description.size() + 1 > 2048) entry.descriptionInvalid = true;
            else if (! entry.descriptionInvalid)
            {
                if (! entry.description.empty()) entry.description += ' ';
                entry.description += content;
            }
            continue;
        }
        entry.continuingDescription = false;
        if (indent == 0 && content.compare (0, 7, "- name:") == 0)
        { finish(); entry = {}; category.clear(); categoryPage = false; }
        else if (indent == 2 && content.compare (0, 5, "page:") == 0)
        { if (categoryPage) return result; category = scalar (content.substr (5)); categoryPage = true; }
        else if (indent == 2 && content.compare (0, 7, "- name:") == 0)
        {
            finish(); entry = {}; entry.active = true; entry.category = category; entry.name = scalar (content.substr (7));
            if (++entries > 4096) return result;
        }
        else if (entry.active && indent == 4)
        {
            if (content == "downloads:") entry.downloads = true;
            else if (content.compare (0, 8, "- label:") != 0) entry.downloads = false;
            if (content.compare (0, 5, "page:") == 0)
            { if (entry.hasPage) entry.invalid = true; entry.page = scalar (content.substr (5)); entry.hasPage = true; }
            if (content.compare (0, 4, "url:") == 0 && entry.hasUrl) entry.invalid = true;
            if (content.compare (0, 8, "license:") == 0)
            {
                auto declared = content.substr (8);
                declared.resize (std::min (declared.size(), declared.find (" #")));
                declared.erase (0, std::min (declared.size(), declared.find_first_not_of (' ')));
                while (! declared.empty() && declared.back() == ' ') declared.pop_back();
                const bool stated = ! declared.empty() && declared != "\"\"" && declared != "''";
                entry.license = stated ? scalar (content.substr (8)) : std::string {};
                if (entry.hasLicense || (stated && (entry.license.empty() || entry.license.size() > 64
                    || ! printable (entry.license)))) entry.licenseUnreadable = true;
                entry.hasLicense = true;
            }
            if (content.compare (0, 18, "short_description:") == 0)
            {
                if (entry.hasDescription) entry.invalid = true;
                entry.hasDescription = entry.continuingDescription = true;
                const auto start = content.find_first_not_of (' ', 18);
                entry.description = start == std::string::npos ? std::string {} : content.substr (start);
                const auto end = entry.description.find (" #");
                auto style = entry.description.substr (0, end);
                while (! style.empty() && style.back() == ' ') style.pop_back();
                entry.descriptionBlock = style == ">" || style == ">-" || style == ">+"
                    || style == "|" || style == "|-" || style == "|+";
                if (entry.descriptionBlock) entry.description.clear();
            }
        }
        else if (entry.active && indent > 4
            && (content.compare (0, 8, "license:") == 0 || content.compare (0, 10, "- license:") == 0))
            entry.licenseUnreadable = true;
        if (! entry.active || ! ((indent == 4 || ((indent == 6 || indent == 8) && entry.downloads))
            && content.compare (0, 4, "url:") == 0)) continue;
        if (indent == 4) entry.hasUrl = true;
        const auto url = scalar (content.substr (4));
        constexpr std::string_view prefix = "https://github.com/sfzinstruments/";
        if (! https (url) || url.compare (0, prefix.size(), prefix) != 0) continue;
        const auto name = url.substr (prefix.size(), url.find_first_of ("/?#", prefix.size()) - prefix.size());
        const auto suffix = url.substr (prefix.size() + name.size());
        if (repositoryName (name) && (suffix.empty() || suffix == "/" || suffix.compare (0, 19, "/releases/download/") == 0))
            entry.repositories.insert (lower (name));
    }
    finish();
    if (described.size() == 1) result.described = std::move (described.front());
    if (! ambiguous && admitted.size() == 1 && ! admitted.front().licenseUnreadable)
        result.declared = std::move (admitted.front());
    return result;
}

OperationResult githubCatalog (HttpTransport& transport, std::string& text, const OperationControl& control)
{
    HttpRequest request;
    request.url = "https://raw.githubusercontent.com/sfzinstruments/sfzinstruments.github.io/master/data/sfz/instruments.yml";
    request.connectTimeoutSeconds = 3; request.timeoutSeconds = 5; request.lowSpeedSeconds = 3;
    try
    {
        const auto catalog = fetchHttpText (transport, request, 131072, control);
        const auto catalogStatus = control.status();
        if (catalogStatus != OperationStatus::Complete) return { catalogStatus, "Source metadata lookup interrupted" };
        if (catalog.status == OperationStatus::Paused || catalog.status == OperationStatus::Cancelled)
            return { catalog.status, catalog.error };
        if (catalog && catalog.response.statusCode == 200) text = catalog.body;
    }
    catch (const std::exception&) {}
    const auto catalogStatus = control.status();
    return { catalogStatus, catalogStatus == OperationStatus::Complete ? std::string {} : "Source metadata lookup interrupted" };
}

OperationResult githubDemos (HttpTransport& transport, const CatalogLookup& catalog,
                             ProviderPresentation& presentation, const OperationControl& control)
{
    const auto& metadata = catalog.described;
    if (! metadata) return { OperationStatus::Complete, {} };
    appendMetadataText (presentation.categoryHint, metadata->description);
    const auto& page = metadata->page;
    HttpRequest request;
    request.url = "https://sfzinstruments.github.io/" + page + "/";
    request.connectTimeoutSeconds = 3; request.timeoutSeconds = 5; request.lowSpeedSeconds = 3;
    const auto html = fetchHttpText (transport, request, 262144, control);
    const auto htmlStatus = control.status();
    if (htmlStatus != OperationStatus::Complete) return { htmlStatus, "Source demo lookup interrupted" };
    if (html.status == OperationStatus::Paused || html.status == OperationStatus::Cancelled)
        return { html.status, html.error };
    if (! html || html.response.statusCode != 200 || html.response.url != request.url)
        return { OperationStatus::Complete, {} };
    const auto audio = "/assets/audio/" + page + ".mp3";
    std::size_t offset = 0;
    while ((offset = html.body.find ('<', offset)) != std::string::npos)
    {
        if (html.body.compare (offset, 4, "<!--") == 0)
        { const auto end = html.body.find ("-->", offset + 4); if (end == std::string::npos) break; offset = end + 3; continue; }
        const auto end = html.body.find ('>', offset + 1);
        if (end == std::string::npos) break;
        const auto tag = html.body.substr (offset, end - offset + 1);
        if (tag.compare (0, 5, "<img ") == 0 && presentation.artworkUrl.empty())
        {
            for (const char quote : { '\'', '"' })
            {
                const auto marker = std::string (" src=") + quote;
                const auto start = tag.find (marker);
                if (start == std::string::npos) continue;
                const auto from = start + marker.size(), to = tag.find (quote, from);
                if (to == std::string::npos) continue;
                const auto path = tag.substr (from, to - from);
                const auto prefix = "/assets/img/" + page.substr (0, page.find ('/')) + "/";
                if (path.compare (0, prefix.size(), prefix) == 0 && path.find ("..") == std::string::npos
                    && https ("https://sfzinstruments.github.io" + path))
                    presentation.artworkUrl = "https://raw.githubusercontent.com/sfzinstruments/sfzinstruments.github.io/master" + path;
            }
        }
        if (tag.compare (0, 7, "<audio ") == 0
            && (tag.find (" src=\"" + audio + "\"") != std::string::npos
                || tag.find (" src='" + audio + "'") != std::string::npos))
        { if (presentation.demos.empty()) presentation.demos.push_back ({ "Listen on SFZ Instruments", request.url }); }
        offset = end + 1;
    }
    return { OperationStatus::Complete, {} };
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

struct LicenseKind
{
    std::string id, slug, version, url, constraints;
};

std::optional<LicenseKind> licenseKind (std::string identifier)
{
    identifier = lower (std::move (identifier));
    if (identifier == "unlicense")
        return LicenseKind { "Unlicense", "unlicense", {}, "https://unlicense.org/",
                             "Unlicense public-domain dedication; no attribution condition." };
    if (identifier == "public-domain")
        return LicenseKind { "Public-Domain", "public-domain", {}, {},
                             "Public domain as declared by the SFZ Instruments catalog; no attribution condition." };
    if (identifier == "unknown")
        return LicenseKind { "Unknown", "unknown", {}, {},
                             "No license is stated. Check the source's terms before you use or share these samples." };
    if (identifier == "other")
        return LicenseKind { "Other", "other", {}, {},
                             "Dusk Studio does not recognize these terms. Read them before you use or share these samples." };
    if (identifier == "cc0-1.0")
        return LicenseKind { "CC0-1.0", "zero", "1.0", "https://creativecommons.org/publicdomain/zero/1.0/",
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
                return LicenseKind { id, type, version,
                    std::string ("https://creativecommons.org/licenses/") + type + "/" + version + "/", constraints };
            }
    return {};
}

std::optional<LicenseKind> artifactLicense (const std::string& shortName)
{
    if (shortName == "cc-zero") return licenseKind ("CC0-1.0");
    auto type = shortName;
    std::string version = "4.0";
    if (type.size() > 2 && type.substr (type.size() - 2) == "-3")
    { type.resize (type.size() - 2); version = "3.0"; }
    return licenseKind ("cc-" + type + "-" + version);
}

bool downloadLicenseAllowed (const LicenseKind& license)
{
    return license.slug == "zero" || license.slug == "by" || license.slug == "by-sa";
}

bool creativeCommons (const LicenseKind& license)
{
    return license.url.compare (0, 28, "https://creativecommons.org/") == 0;
}

constexpr const char* kRestrictedLicense =
    "NonCommercial and NoDerivatives packs are unavailable. Choose CC0, CC BY or CC BY-SA content.";

bool formalLicenseTitle (const std::string& text, const LicenseKind& license)
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

std::optional<LicenseKind> licenseText (const std::string& text, const std::string& hint, bool wholePack = true)
{
    const auto content = lower (text);
    for (const auto* conflict : { "all rights reserved", "not covered by this license", "separate license",
                                  "licensed separately", "mit license", "gnu general public license" })
        if (content.find (conflict) != std::string::npos) return {};
    auto scope = content;
    scope.erase (std::remove_if (scope.begin(), scope.end(), [] (char c) { return c == '*' || c == '_'; }), scope.end());
    if (wholePack)
        for (const auto* label : { "scripts:", "code:", "source code:", "software:", "samples:", "recordings:",
                                   "scripts are licensed", "code is licensed", "software is licensed" })
            if (scope.find (label) != std::string::npos) return {};
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
        || content.find ("cc0 1.0 universal") != std::string::npos
        || (content.find ("open cc0 general-purpose sample library") != std::string::npos
            && content.find ("this collection is under a creative commons 0 license.") != std::string::npos))
        found.insert ("CC0-1.0");
    for (const auto* type : types)
        for (const auto* version : versions)
        {
            const auto license = licenseKind (std::string ("cc-") + type + "-" + version);
            const auto marker = std::string ("//creativecommons.org/licenses/") + type + "/" + version;
            if (containsUrl (marker) || formalLicenseTitle (text, *license)) found.insert (license->id);
        }
    if (found.size() != 1) return {};
    const auto actual = licenseKind (*found.begin());
    const auto claimed = licenseKind (hint);
    if (claimed && claimed->id != actual->id) return {};
    const auto identifierChar = [] (char c)
    { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'; };
    for (std::size_t begin = 0; begin < content.size();)
    {
        if (! identifierChar (content[begin])) { ++begin; continue; }
        auto end = begin + 1;
        while (end < content.size() && identifierChar (content[end])) ++end;
        auto identifier = content.substr (begin, end - begin);
        if (identifier.compare (0, 3, "cc-") == 0 || identifier.compare (0, 4, "cc0-") == 0)
            while (! identifier.empty() && identifier.back() == '-')
            {
                auto next = content.find_first_not_of (" \t\r\n", end);
                if (next == std::string::npos || ! identifierChar (content[next])) break;
                auto nextEnd = next;
                while (nextEnd < content.size() && identifierChar (content[nextEnd])) ++nextEnd;
                identifier += content.substr (next, nextEnd - next);
                end = nextEnd;
            }
        while (! identifier.empty() && identifier.back() == '.') identifier.pop_back();
        if (identifier.compare (0, 5, "cc-by") == 0
            || (identifier.size() > 4 && identifier.compare (0, 4, "cc0-") == 0))
        {
            const auto declared = licenseKind (identifier);
            if (! declared || declared->id != actual->id) return {};
        }
        begin = end;
    }
    if (downloadLicenseAllowed (*actual))
    {
        std::string wording { " " };
        wording.reserve (content.size() + 2);
        for (const auto c : content)
        {
            const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (word || wording.back() != ' ') wording += word ? c : ' ';
        }
        wording += ' ';
        for (const auto* restriction : { "noncommercial", "non commercial", "noderivatives", "noderivs",
                                        " cc by nc ", " cc by nd ", " creative commons by nc ", " creative commons by nd ",
                                        "commercial use is not permitted",
                                        "no derivatives", "no derivative works", "no modifications",
                                        "no commercial use", "not for commercial", "personal use only" })
            if (wording.find (restriction) != std::string::npos) return {};
        // A bare "BY-NC" is an author credit ("by NC Audio") unless a version or
        // another licence element follows it.
        for (const std::string restricted : { " by nc ", " by nd " })
            for (auto at = wording.find (restricted); at != std::string::npos; at = wording.find (restricted, at + 1))
            {
                const auto next = wording.substr (at + restricted.size(), 3);
                if ((! next.empty() && next[0] >= '0' && next[0] <= '9')
                    || next == "sa " || next == "nc " || next == "nd ") return {};
            }
        // Hyphenated, it is licence notation even with nothing after it
        // ("Samples: BY-NC."): the hyphen is what a credit never has. Any dash
        // counts, from the hyphen-minus through U+2010-2014 to the minus sign.
        const auto letter = [] (char c) { return c >= 'a' && c <= 'z'; };
        const auto digit = [] (char c) { return c >= '0' && c <= '9'; };
        for (const std::string dash : { "-", "\xe2\x80\x90", "\xe2\x80\x91", "\xe2\x80\x92", "\xe2\x80\x93",
                                        "\xe2\x80\x94", "\xe2\x88\x92" })
            for (const std::string element : { "nc", "nd" })
            {
                const auto token = "by" + dash + element;
                for (auto at = content.find (token); at != std::string::npos; at = content.find (token, at + 1))
                {
                    const auto end = at + token.size();
                    if ((at == 0 || ! (letter (content[at - 1]) || digit (content[at - 1])))
                        && (end == content.size() || ! letter (content[end])))
                        return {};
                }
            }
    }
    return actual;
}

constexpr std::string_view kUnlicense = R"unlicense(This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <https://unlicense.org>
)unlicense";

std::string collapsedWhitespace (std::string_view text)
{
    std::string result;
    for (const auto c : text)
    {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') result += c;
        else if (! result.empty() && result.back() != ' ') result += ' ';
    }
    if (! result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

bool unlicenseText (std::string_view text)
{
    auto collapsed = collapsedWhitespace (text);
    for (const std::string_view link : { "<http://unlicense.org>", "<http://unlicense.org/>", "<https://unlicense.org/>" })
        if (collapsed.size() >= link.size() && collapsed.compare (collapsed.size() - link.size(), link.size(), link) == 0)
        {
            collapsed.replace (collapsed.size() - link.size(), link.size(), "<https://unlicense.org>");
            break;
        }
    return collapsed == collapsedWhitespace (kUnlicense);
}

std::optional<LicenseKind> fileLicense (const std::string& text, const std::string& hint)
{
    if (unlicenseText (text) && (hint.empty() || hint == "NOASSERTION" || lower (hint) == "unlicense"))
        return licenseKind ("Unlicense");
    return licenseText (text, hint);
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
    const auto code = response.response.statusCode;
    result.rateLimit.limited = code == 429 || (code == 403 && result.rateLimit.remaining == "0")
        || (code == 403 && ! html && ! result.rateLimit.retryAfter.empty());
    if (result.rateLimit.limited)
        result.error = "Repository rate limit reached; the source link remains available.";
    else if (html)
        result.error = "Repository returned a browser or anti-bot page. Open the source website; automated downloads are unavailable.";
    else if (code == 403)
        result.error = "Repository denied API access. Try again later or open the source website.";
    else if (code != 200)
        result.error = "Repository request failed (HTTP " + std::to_string (code) + ").";
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

ProviderLicense catalogLicense (const CatalogLookup& catalog, const std::string& treeUrl)
{
    const auto value = catalog.declared ? catalog.declared->license : std::string {};
    const auto name = lower (value);
    if (name.empty() || name == "unknown")
    {
        const auto unknown = licenseKind ("Unknown");
        const auto text = "Unknown\nNo license Dusk Studio can read was found at " + treeUrl + " or in the SFZ Instruments catalog.";
        return { unknown->id, text, {}, sha256 (text), treeUrl, unknown->constraints };
    }
    auto kind = licenseKind (name == "public domain" ? "Public-Domain" : value);
    if (! kind) kind = licenseKind ("Other");
    // The declared value and URLs only: other catalog edits must not change the download's resume key.
    const auto page = "https://sfzinstruments.github.io/" + catalog.declared->page + "/";
    const auto text = (kind->id == "Other" ? value : kind->id) + "\nDeclared in the SFZ Instruments catalog: " + page
        + "\nRepository: " + treeUrl;
    return { kind->id, text, {}, sha256 (text), page, kind->constraints };
}

ProviderPackResult githubPack (HttpTransport& transport, const ProviderSummary& summary,
                               const OperationControl& control)
{
    ProviderPackResult result;
    const auto fail = [&] (const std::string& error)
    { result.status = OperationStatus::Failed; result.error = error; return result; };
    const auto* owner = githubOwner (summary.sourceId);
    if (owner == nullptr || ! githubRepositoryAllowed (summary.sourceId, summary.locator)
        || ! decimalId (summary.packId)) return fail ("Invalid GitHub SFZ repository locator");
    const auto base = "https://api.github.com/repos/" + std::string (owner) + "/" + summary.locator;
    const auto repository = api (transport, base, 1048576, control);
    result.rateLimit = repository.rateLimit;
    if (! repository) { result.status = repository.status; result.error = repository.error; return result; }
    if (std::to_string (integer (repository.json, "id")) != summary.packId
        || lower (string (dusk::json::child (repository.json, "owner"), "login", 100)) != owner)
        return fail ("Repository identity changed; refresh the source listing");
    std::string revisionRef = "HEAD";
    if (summary.sourceId == "versilian")
    {
        // The default branches contain raw samples; published SFZ releases include the mappings.
        const auto release = api (transport, base + "/releases/latest", 1048576, control);
        result.rateLimit = release.rateLimit;
        if (! release) { result.status = release.status; result.error = release.error; return result; }
        revisionRef = string (release.json, "tag_name", 128);
        const auto draft = release.json.find ("draft"), prerelease = release.json.find ("prerelease");
        if (revisionRef.empty() || draft == release.json.end() || ! draft->is_boolean() || draft->get<bool>()
            || prerelease == release.json.end() || ! prerelease->is_boolean() || prerelease->get<bool>())
            return fail ("The repository has no published stable SFZ release");
    }
    const auto commit = api (transport, base + "/commits/" + encode (revisionRef), 1048576, control);
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
    pack.sourceId = summary.sourceId;
    pack.packId = summary.packId;
    pack.releaseId = revision;
    pack.displayName = summary.locator;
    pack.author = summary.sourceId == "freepats" ? "FreePats"
        : summary.sourceId == "versilian" ? "Versilian Studios" : "SFZ Instruments";
    const auto webBase = "https://github.com/" + std::string (owner) + "/" + summary.locator;
    pack.sourceUrl = webBase + "/tree/" + revision;
    pack.downloadUrl = webBase + "/archive/" + revision + ".zip";
    pack.expectedRoot = summary.locator + "-" + revision;
    pack.versionPinned = true;
    presentationMetadata (repository.json, pack.presentation, summary.sourceId);
    if (summary.sourceId == "sfzinstruments")
        appendMetadataText (pack.presentation.categoryHint, summary.presentation.categoryHint);
    // SFZ Instruments packs download whatever license they carry; it is resolved only to show it.
    const bool sfzInstruments = summary.sourceId == "sfzinstruments";
    std::set<std::string> paths;
    std::set<std::string> regularFiles;
    std::set<std::string> licensePaths, licenseKeys;
    for (const auto& entry : *entries)
    {
        const auto mode = string (entry, "mode", 16);
        const auto path = string (entry, "path", 512);
        const auto type = string (entry, "type", 16);
        if (type == "tree" && mode == "040000")
        {
            std::string error;
            if (! validateRelativePath (path, error)) return fail ("Repository directory path is unsafe");
            continue;
        }
        const bool regular = type == "blob" && (mode == "100644" || mode == "100755");
        if (! sfzInstruments && isPackLicenseDocument (path))
        {
            std::string error;
            const auto key = portablePathKey (path);
            if (! regular || ! validateRelativePath (path, error) || ! key
                || ! licenseKeys.insert (*key).second || licensePaths.size() >= kMaxLicenseDocuments)
                return fail ("Repository licensing paths are unsafe, ambiguous, or too numerous");
            licensePaths.insert (path);
        }
        if (! regular) continue;
        regularFiles.insert (path);
        if (sfzPath (path) && ! addInstrument (pack, path, paths)) return fail ("Repository has unsafe or ambiguous SFZ paths");
    }
    if (pack.instruments.empty()) return fail ("Repository contains no SFZ files at the pinned revision");
    if (sfzInstruments && std::none_of (regularFiles.begin(), regularFiles.end(), [] (const std::string& path)
    {
        const auto name = lower (path);
        const auto dot = name.find_last_of ('.');
        const auto extension = dot == std::string::npos || name.find ('/', dot) != std::string::npos
            ? std::string {} : name.substr (dot);
        return extension == ".wav" || extension == ".flac" || extension == ".ogg"
            || extension == ".aif" || extension == ".aiff";
    })) return fail ("This repository has SFZ files but no samples to download.");
    for (const auto& image : regularFiles)
    {
        if (! pack.presentation.artworkUrl.empty()) break;
        const auto name = lower (image);
        const auto extension = name.substr (name.find_last_of ('.') == std::string::npos ? name.size() : name.find_last_of ('.'));
        if (extension != ".png" && extension != ".jpg" && extension != ".jpeg") continue;
        const auto leaf = name.substr (name.find_last_of ('/') == std::string::npos ? 0 : name.find_last_of ('/') + 1);
        if (leaf != "cover" + extension && leaf != "instrument" + extension) continue;
        if (! isInstrumentArtworkPath (name)) continue;
        pack.presentation.artworkUrl = "https://raw.githubusercontent.com/" + std::string (owner)
            + "/" + summary.locator + "/" + revision + "/" + encode (image, true);
    }
    // GitHub's license detection also covers a root UNLICENSE file. The VCSL SFZ release predates its
    // LICENSE file; its archived README carries the CC0 grant.
    const bool hasLicenseFile = std::any_of (regularFiles.begin(), regularFiles.end(), [] (const std::string& path)
    {
        const auto name = lower (path);
        return name.find ('/') == std::string::npos
            && (isPackLicenseDocument (name) || name.compare (0, 9, "unlicense") == 0);
    });
    const bool readmeLicense = summary.sourceId == "versilian" && summary.locator == "VCSL"
        && ! hasLicenseFile && regularFiles.count ("README.md") != 0;
    if (! sfzInstruments && ! hasLicenseFile && ! readmeLicense)
        return fail ("This repository has no license file, so Dusk Studio cannot check its terms. Open the source website.");
    if (hasLicenseFile || readmeLicense)
    {
        const auto license = api (transport,
            base + (readmeLicense ? "/contents/README.md?ref=" : "/license?ref=") + revision, 400000, control);
        result.rateLimit = license.rateLimit;
        const bool notFound = license.status == OperationStatus::Failed && license.response.statusCode == 404;
        if (! license && ! (sfzInstruments && notFound))
        {
            result.status = license.status;
            result.error = ! readmeLicense && notFound
                ? "GitHub could not identify this repository's license file. Open the source website for its terms."
                : license.error;
            return result;
        }
        if (license)
        {
            const auto path = string (license.json, "path", 512);
            const auto encoded = string (license.json, "content", 300000);
            const auto hint = string (dusk::json::child (license.json, "license"), "spdx_id", 100);
            const bool base64 = string (license.json, "encoding", 16) == "base64" && ! encoded.empty();
            std::string pathError;
            if (! validateRelativePath (path, pathError) || regularFiles.count (path) == 0 || (! sfzInstruments && ! base64))
                return fail ("Repository LICENSE file cannot be verified at the pinned revision");
            std::string text (kMaxLicenseBytes, '\0');
            std::size_t decoded = 0;
            const bool decodable = base64 && sodium_init() >= 0
                && sodium_base642bin (reinterpret_cast<unsigned char*> (text.data()), text.size(), encoded.data(),
                    encoded.size(), " \t\r\n", &decoded, nullptr, sodium_base64_VARIANT_ORIGINAL) == 0;
            text.resize (decodable ? decoded : 0);
            const bool readable = decodable && text.find ('\0') == std::string::npos && portablePathKey (text);
            if (! sfzInstruments && ! decodable) return fail ("Repository LICENSE content is invalid or too large");
            if (! sfzInstruments && ! readable) return fail ("Repository LICENSE is not valid text");
            if (readable)
            {
                auto recognized = sfzInstruments ? fileLicense (text, hint) : licenseText (text, hint);
                if (! sfzInstruments && ! recognized)
                    return fail ("Repository LICENSE does not establish one supported Creative Commons license");
                if (! sfzInstruments && ! downloadLicenseAllowed (*recognized)) return fail (kRestrictedLicense);
                if (! recognized) recognized = licenseKind ("Other");
                pack.license = { recognized->id, text, path, sha256 (text),
                    webBase + "/blob/" + revision + "/" + encode (path, true), recognized->constraints };
                licensePaths.insert (path);
            }
        }
    }
    if (licensePaths.size() > kMaxLicenseDocuments) return fail ("Repository contains too many licensing documents");
    std::size_t licenseBytes = pack.license.text.size();
    for (const auto& secondaryPath : licensePaths)
    {
        if (secondaryPath == pack.license.path) continue;
        const auto document = api (transport, base + "/contents/" + encode (secondaryPath, true) + "?ref=" + revision,
                                   400000, control);
        result.rateLimit = document.rateLimit;
        if (! document) { result.status = document.status; result.error = document.error; return result; }
        const auto encodedText = string (document.json, "content", 300000);
        if (string (document.json, "path", 512) != secondaryPath
            || string (document.json, "encoding", 16) != "base64" || encodedText.empty())
            return fail ("Repository licensing document cannot be verified at the pinned revision");
        std::string secondaryText (kMaxLicenseBytes, '\0');
        std::size_t size = 0;
        if (sodium_base642bin (reinterpret_cast<unsigned char*> (secondaryText.data()), secondaryText.size(),
            encodedText.data(), encodedText.size(), " \t\r\n", &size, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0)
            return fail ("Repository licensing document is invalid or too large");
        secondaryText.resize (size);
        if (size > kMaxCombinedLicenseBytes - licenseBytes)
            return fail ("Repository licensing documents exceed the combined size limit");
        licenseBytes += size;
        const auto checked = validatePackLicenseDocument (pack.license, secondaryPath, secondaryText);
        if (! checked) return fail (checked.error + ": " + secondaryPath);
    }
    std::optional<CatalogLookup> catalog;
    if (sfzInstruments && pack.license.id.empty())
    {
        std::string text;
        const auto fetched = githubCatalog (transport, text, control);
        if (! fetched) { result.status = fetched.status; result.error = fetched.error; return result; }
        catalog = catalogLookup (text, summary.locator);
        pack.license = catalogLicense (*catalog, pack.sourceUrl);
    }
    std::sort (pack.instruments.begin(), pack.instruments.end(), [] (const auto& a, const auto& b)
    { return a.relativePath < b.relativePath; });
    pack.presentation.stars = popularity (repository.json, "stargazers_count");
    try
    {
        if (sfzInstruments)
        {
            if (! catalog)
            {
                std::string text;
                const auto fetched = githubCatalog (transport, text, control);
                if (! fetched) { result.status = fetched.status; result.error = fetched.error; return result; }
                catalog = catalogLookup (text, summary.locator);
            }
            const auto demo = githubDemos (transport, *catalog, pack.presentation, control);
            if (! demo) { result.status = demo.status; result.error = demo.error; return result; }
        }
    }
    catch (const std::exception&)
    {
        const auto status = control.status();
        if (status != OperationStatus::Complete)
        { result.status = status; result.error = "Source demo lookup interrupted"; return result; }
    }
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
    if (! downloadLicenseAllowed (*recognized)) return fail (kRestrictedLicense);
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
            if (! licenseText (name, recognized->id))
                return fail ("Artifact license definition contains unsupported or conflicting terms");
            if (! declaration.empty() && declaration != name)
                return fail ("Artifact license definitions are ambiguous");
            declaration = name;
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
    presentationMetadata (detail.json, pack.presentation, "musical-artifacts");
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
    pack.presentation.favorites = popularity (detail.json, "favorite_count");
    artifactDemos (detail.json, pack.presentation);
    result.pack = std::move (pack);
    result.status = OperationStatus::Complete;
    return result;
}
} // namespace

bool isPackLicenseDocument (std::string_view path)
{
    const auto name = lower (std::string (path));
    std::size_t begin = 0;
    for (;;)
    {
        const auto end = name.find ('/', begin);
        const auto component = name.substr (begin, end - begin);
        if (end != std::string::npos && (component == "licenses" || component == "licences"
            || component == "license" || component == "licence" || component == "copying")) return true;
        if (end == std::string::npos)
        {
            for (const std::string_view prefix : { "license", "licence", "copying" })
                if (component.compare (0, prefix.size(), prefix) == 0
                    && (component.size() == prefix.size() || component[prefix.size()] == '.'
                        || component[prefix.size()] == '-' || component[prefix.size()] == '_')) return true;
            for (const std::string_view suffix : { ".license", ".licence" })
                if (component.size() >= suffix.size()
                    && component.compare (component.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
            return false;
        }
        begin = end + 1;
    }
}

OperationResult validatePackLicenseDocument (const ProviderLicense& primary, std::string_view path, std::string_view text)
{
    if (text.empty() || text.size() > kMaxLicenseBytes || text.find ('\0') != std::string_view::npos
        || ! portablePathKey (text)) return { OperationStatus::Failed, "Pack licensing document is not bounded UTF-8 text" };
    if (! primary.path.empty() && path == primary.path)
    {
        if (sha256 (text) != primary.sha256)
            return { OperationStatus::Failed, "The extracted primary license differs from the resolved document" };
    }
    else
    {
        const auto license = licenseText (std::string (text), primary.id, false);
        if (! license || ! downloadLicenseAllowed (*license))
            return { OperationStatus::Failed, "Pack licensing document has unsupported, restricted, or conflicting terms" };
    }
    return { OperationStatus::Complete, {} };
}

std::vector<ProviderSource> providerSources()
{
    return { { "sfzinstruments", "SFZ Instruments", "https://sfzinstruments.github.io/" },
             { "musical-artifacts", "Musical Artifacts", "https://musical-artifacts.com/" },
             { "freepats", "FreePats", "https://freepats.zenvoid.org/" },
             { "versilian", "Versilian Studios", "https://versilian-studios.com/" } };
}

std::vector<std::string> artworkHostsForSource (std::string_view source)
{
    if (source == "musical-artifacts") return { "musical-artifacts.com", "www.musical-artifacts.com" };
    if (source == "sfzinstruments" || source == "freepats" || source == "versilian")
        return { "raw.githubusercontent.com", "user-images.githubusercontent.com", "repository-images.githubusercontent.com",
                 "avatars.githubusercontent.com" };
    return {};
}

bool isInstrumentArtworkPath (std::string_view path)
{
    const auto scheme = path.find ("://");
    if (scheme != std::string_view::npos)
    {
        const auto slash = path.find ('/', scheme + 3);
        path = slash == std::string_view::npos ? std::string_view {} : path.substr (slash);
    }
    path = path.substr (0, path.find_first_of ("?#"));
    const auto hex = [] (char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string decoded;
    for (std::size_t i = 0; i < path.size(); ++i)
    {
        if (path[i] == '%' && i + 2 < path.size() && hex (path[i + 1]) >= 0 && hex (path[i + 2]) >= 0)
        {
            decoded += static_cast<char> (hex (path[i + 1]) * 16 + hex (path[i + 2]));
            i += 2;
        }
        else decoded += path[i];
    }
    if (instrumentWordsContain (instrumentWords (decoded),
        { "logo", "banner", "diagram", "waveform", "screenshot", "knob" })) return false;
    for (std::size_t at = 0; at < decoded.size();)
    {
        const auto slash = decoded.find ('/', at);
        const auto words = instrumentWords (decoded.substr (at, slash == std::string::npos ? slash : slash - at));
        if (words.rfind (" gui ", 0) == 0 || words.rfind (" skin ", 0) == 0 || words.rfind (" skins ", 0) == 0) return false;
        auto controlWords = words;
        for (const auto instrument : { std::string_view (" button accordion "), std::string_view (" button accordions ") })
            for (auto pos = controlWords.find (instrument); pos != std::string::npos; pos = controlWords.find (instrument, pos))
                controlWords.replace (pos, instrument.size(), " ");
        if (instrumentWordsContain (controlWords, { "button" })) return false;
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    return true;
}

ProviderPageResult browseProvider (HttpTransport& transport, const std::string& sourceId,
                                   unsigned page, const std::string& query, const OperationControl& control)
{
    ProviderPageResult result;
    const auto* owner = githubOwner (sourceId);
    if (page == 0 || page > 200 || query.size() > 256
        || (owner == nullptr && sourceId != "musical-artifacts"))
    { result.error = "Invalid repository browse request"; return result; }
    const bool github = owner != nullptr;
    const auto url = github
        ? "https://api.github.com/" + std::string (sourceId == "sfzinstruments" ? "orgs/" : "users/")
            + owner + "/repos?type=" + (sourceId == "sfzinstruments" ? "public" : "owner")
            + "&sort=full_name&per_page=50&page=" + std::to_string (page)
        : "https://musical-artifacts.com/artifacts.json?formats=sfz&page=" + std::to_string (page) + "&q=" + encode (query);
    const auto response = api (transport, url, 2 * 1048576, control);
    result.rateLimit = response.rateLimit;
    if (! response) { result.status = response.status; result.error = response.error; return result; }
    if (! response.json.is_array() || response.json.size() > 256)
    { result.error = "Repository listing is invalid or too large"; return result; }
    std::string catalog;
    if (sourceId == "sfzinstruments" && ! query.empty())
    {
        const auto metadata = githubCatalog (transport, catalog, control);
        if (! metadata) { result.status = metadata.status; result.error = metadata.error; return result; }
    }
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
        presentationMetadata (entry, summary.presentation, sourceId);
        if (github)
        {
            summary.presentation.stars = popularity (entry, "stargazers_count");
            summary.locator = summary.name;
            summary.author = string (dusk::json::child (entry, "owner"), "login", 100);
            const auto disabled = entry.find ("disabled"), privateRepo = entry.find ("private");
            if (! githubRepositoryAllowed (sourceId, summary.locator) || lower (summary.author) != owner
                || (disabled != entry.end() && disabled->is_boolean() && disabled->get<bool>())
                || (privateRepo != entry.end() && privateRepo->is_boolean() && privateRepo->get<bool>())) continue;
            summary.sourceUrl = "https://github.com/" + std::string (owner) + "/" + summary.locator;
            summary.licenseHint = string (dusk::json::child (entry, "license"), "spdx_id", 100);
            const auto license = licenseKind (summary.licenseHint);
            if (sourceId != "sfzinstruments" && (! license || ! downloadLicenseAllowed (*license))) continue;
            if (! catalog.empty())
            {
                const auto metadata = catalogLookup (catalog, summary.locator).described;
                if (metadata)
                {
                    appendMetadataText (summary.description, metadata->description);
                    appendMetadataText (summary.presentation.categoryHint, metadata->description);
                }
            }
            if (sourceId == "versilian")
            {
                summary.name = summary.locator == "VCSL" ? "Versilian Community Sample Library" : "VSCO 2 Community Edition";
                summary.description += summary.locator == "VCSL"
                    ? " SFZ keyboards, piano, organ, woodwinds, strings and percussion. Round-robin sampling varies by instrument."
                    : " Official SFZ orchestra: strings, brass, woodwinds, piano and percussion, with round-robin short articulations.";
            }
            if (! query.empty() && lower (summary.name + " " + summary.description).find (lower (query)) == std::string::npos) continue;
        }
        else
        {
            summary.presentation.favorites = popularity (entry, "favorite_count");
            summary.locator = summary.packId;
            summary.author = string (entry, "author", 256);
            const auto license = artifactLicense (string (entry, "license", 100));
            if (! license || ! downloadLicenseAllowed (*license) || ! declaresSfz (entry)) continue;
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
    if (githubOwner (summary.sourceId) != nullptr) return githubPack (transport, summary, control);
    if (summary.sourceId == "musical-artifacts") return artifactPack (transport, summary, control);
    ProviderPackResult result;
    result.error = "Unknown SFZ repository";
    return result;
}

std::string providerPackToJson (const ProviderPack& pack)
{
    // Optional presentation must not change provider identity or resumable-download keys.
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

ProviderPackResult providerPackFromJson (std::string_view source, PackDescriptorUse use)
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
    const bool sfzInstruments = pack.sourceId == "sfzinstruments";
    const auto recognized = licenseKind (pack.license.id);
    if (use == PackDescriptorUse::Download && ! sfzInstruments && recognized && creativeCommons (*recognized)
        && ! downloadLicenseAllowed (*recognized))
        return fail (kRestrictedLicense);
    std::string pathError;
    if (! decimalId (pack.packId) || pack.displayName.empty() || ! https (pack.sourceUrl) || ! https (pack.downloadUrl)
        || ! https (pack.license.sourceUrl) || ! recognized || (! sfzInstruments && ! creativeCommons (*recognized))
        || pack.license.text.empty()
        || ! isSha256 (pack.license.sha256) || sha256 (pack.license.text) != pack.license.sha256
        || (! pack.expectedSha256.empty() && ! isSha256 (pack.expectedSha256))
        || (! pack.expectedRoot.empty() && (pack.expectedRoot.find ('/') != std::string::npos
                                            || ! validateRelativePath (pack.expectedRoot, pathError)))
        || (! pack.license.path.empty() && ! validateRelativePath (pack.license.path, pathError)))
        return fail ("Cached SFZ descriptor has invalid identity, license, URL, or paths");
    if (const auto* owner = githubOwner (pack.sourceId))
    {
        const auto prefix = "https://github.com/" + std::string (owner) + "/";
        const auto suffix = pack.downloadUrl.substr (std::min (pack.downloadUrl.size(), prefix.size()));
        const auto repository = suffix.substr (0, suffix.find ('/'));
        const auto& terms = pack.license.sourceUrl;
        const auto stated = [&pack] (const std::string& value) { return pack.license.text.find (value) != std::string::npos; };
        const auto declaration = pack.license.text.substr (0, pack.license.text.find ('\n'));
        bool licensed = false;
        if (! pack.license.path.empty())
            licensed = recognized->id != "Public-Domain" && recognized->id != "Unknown"
                && terms == prefix + repository + "/blob/" + pack.releaseId + "/" + encode (pack.license.path, true)
                && (sfzInstruments || use != PackDescriptorUse::Download || licenseText (pack.license.text, pack.license.id));
        else if (sfzInstruments && recognized->id == "Unknown")
            licensed = terms == pack.sourceUrl && declaration == "Unknown" && stated (pack.sourceUrl);
        else if (sfzInstruments)
        {
            constexpr std::string_view site = "https://sfzinstruments.github.io/";
            const auto page = terms.size() > site.size() + 1 && terms.compare (0, site.size(), site) == 0
                && terms.back() == '/' ? terms.substr (site.size(), terms.size() - site.size() - 1) : std::string {};
            const auto slash = page.find ('/');
            const auto segment = [] (const std::string& value)
            { return repositoryName (value) && value.find ('.') == std::string::npos; };
            licensed = slash != std::string::npos && segment (page.substr (0, slash)) && segment (page.substr (slash + 1))
                && (recognized->id == "Other" ? ! declaration.empty() : declaration == recognized->id)
                && stated (terms) && stated (pack.sourceUrl);
        }
        if (! gitSha (pack.releaseId) || ! pack.versionPinned || ! pack.expectedSha256.empty()
            || ! githubRepositoryAllowed (pack.sourceId, repository)
            || pack.downloadUrl != prefix + repository + "/archive/" + pack.releaseId + ".zip"
            || pack.expectedRoot != repository + "-" + pack.releaseId
            || pack.sourceUrl != prefix + repository + "/tree/" + pack.releaseId || ! licensed)
            return fail ("Cached GitHub SFZ version is not pinned and licensed");
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
            || (use == PackDescriptorUse::Download && ! licenseText (pack.license.text, pack.license.id))
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

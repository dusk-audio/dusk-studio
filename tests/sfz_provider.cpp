#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzProvider.h"
#include "foundation/Fs.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <array>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace duskstudio::sfz;
using Json = nlohmann::json;
const std::string kCommit (40, 'a');
const std::string kTree (40, 'b');
const std::string kArchiveHash (64, 'c');
const std::string kLicenseText = "Creative Commons Attribution 4.0 International Public License\n"
    "https://creativecommons.org/licenses/by/4.0/\n";

struct LicenseExample
{
    const char* artifactName;
    const char* id;
    const char* title;
    bool allowed;
};
constexpr LicenseExample kLicenseExamples[] {
    { "cc-zero", "CC0-1.0", "Creative Commons CC0 1.0 Universal", true },
    { "by", "CC-BY-4.0", "Creative Commons Attribution 4.0", true },
    { "by-sa-3", "CC-BY-SA-3.0", "Creative Commons Attribution-ShareAlike 3.0", true },
    { "by-nc", "CC-BY-NC-4.0", "Creative Commons Attribution-NonCommercial 4.0", false },
    { "by-nd", "CC-BY-ND-4.0", "Creative Commons Attribution-NoDerivs 4.0", false },
    { "by-nc-sa", "CC-BY-NC-SA-4.0", "Creative Commons Attribution-NonCommercial-ShareAlike 4.0", false },
    { "by-nc-nd", "CC-BY-NC-ND-4.0", "Creative Commons Attribution-NonCommercial-NoDerivs 4.0", false }
};

struct Reply
{
    Json json;
    int status = 200;
    HttpHeaders headers {};
    std::string raw {};
    std::function<void()> beforeBody {};
};

class FakeTransport final : public HttpTransport
{
public:
    OperationResult perform (const HttpRequest& request, const HttpHeadersCallback& headers,
                             const HttpBodyCallback& body, const OperationControl& control) override
    {
        requests.push_back (request);
        if (requests.size() > replies.size()) return { OperationStatus::Failed, "Unscripted request" };
        const auto& reply = replies[requests.size() - 1];
        const auto stopped = [&]
        {
            const auto state = control.status();
            return OperationResult { state == OperationStatus::Complete ? OperationStatus::Failed : state, "Stopped" };
        };
        if (control.status() != OperationStatus::Complete) return stopped();
        if (! headers ({ reply.status, request.url, reply.headers })) return stopped();
        if (reply.beforeBody) reply.beforeBody();
        if (control.status() != OperationStatus::Complete) return stopped();
        const auto bytes = reply.raw.empty() ? reply.json.dump() : reply.raw;
        if (! body (bytes)) return stopped();
        return { OperationStatus::Complete, {} };
    }
    std::vector<Reply> replies;
    std::vector<HttpRequest> requests;
};

std::string base64 (const std::string& text)
{
    REQUIRE (sodium_init() >= 0);
    std::string result (sodium_base64_encoded_len (text.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    REQUIRE (sodium_bin2base64 (result.data(), result.size(), reinterpret_cast<const unsigned char*> (text.data()),
                               text.size(), sodium_base64_VARIANT_ORIGINAL) != nullptr);
    result.resize (result.find ('\0'));
    return result;
}

Json repository (unsigned id = 42, const std::string& name = "Grand-Piano", const std::string& license = "CC-BY-4.0")
{
    return { { "id", id }, { "name", name }, { "description", "A piano instrument" },
             { "owner", { { "login", "sfzinstruments" } } }, { "private", false }, { "disabled", false },
             { "license", { { "spdx_id", license } } } };
}

ProviderSummary githubSummary()
{
    return { "sfzinstruments", "42", "Grand-Piano", "Grand-Piano", "SFZ Instruments", {},
             "https://github.com/sfzinstruments/Grand-Piano", "CC-BY-4.0" };
}

std::vector<Reply> githubReplies (const std::string& license = kLicenseText)
{
    return {
        { repository() },
        { { { "sha", kCommit }, { "commit", { { "tree", { { "sha", kTree } } } } } } },
        { { { "truncated", false }, { "tree", Json::array ({
            { { "path", "LICENSE" }, { "type", "blob" }, { "mode", "100644" } },
            { { "path", "Instruments/Piano.sfz" }, { "type", "blob" }, { "mode", "100644" } },
            { { "path", "Includes/labels.sfz" }, { "type", "blob" }, { "mode", "100644" } },
            { { "path", "Samples/a.wav" }, { "type", "blob" }, { "mode", "100644" } }
        }) } } },
        { { { "path", "LICENSE" }, { "encoding", "base64" }, { "content", base64 (license) },
             { "license", { { "spdx_id", "CC-BY-4.0" } } } } }
    };
}

Json artifact (unsigned id = 77, const std::string& license = "by")
{
    return { { "id", id }, { "name", "Community Piano" }, { "author", "Sampler" }, { "license", license },
             { "description", "A Creative Commons piano" }, { "formats", Json::array ({ "sfz", "zip" }) },
             { "file", "https://musical-artifacts.com/artifacts/" + std::to_string (id) + "/piano.zip" },
             { "file_hash", kArchiveHash }, { "file_id", 99 }, { "file_size", 1024 },
             { "file_list", Json::array ({ "Piano/Piano.sfz", "Piano/Samples/a.wav" }) } };
}

Json artifactLicenses (const std::string& shortName = "by", const std::string& name = "Creative Commons Attribution 4.0")
{
    return Json::array ({ { { "short_name", shortName }, { "name", name }, { "license_type", "cc" } } });
}

ProviderSummary artifactSummary()
{
    return { "musical-artifacts", "77", "77", "Community Piano", "Sampler", {},
             "https://musical-artifacts.com/artifacts/77", "CC-BY-4.0" };
}

ProviderPack resolvedGithub()
{
    FakeTransport transport;
    transport.replies = githubReplies();
    const auto result = resolveProviderPack (transport, githubSummary());
    REQUIRE (result);
    REQUIRE (result.pack);
    return *result.pack;
}
} // namespace

TEST_CASE ("SFZ providers expose external repository sources", "[sfz][provider]")
{
    const auto sources = providerSources();
    REQUIRE (sources.size() == 4);
    CHECK (sources[0].id == "sfzinstruments");
    CHECK (sources[1].id == "musical-artifacts");
    CHECK (sources[2].id == "freepats");
    CHECK (sources[3].id == "versilian");
}

TEST_CASE ("Additional SFZ repositories browse only their own Creative Commons collections", "[sfz][provider]")
{
    std::string source = "freepats", owner = "freepats", name = "world-percussion", query = "percussion";
    SECTION ("FreePats offers its CC instruments") {}
    SECTION ("Versilian offers the official orchestra")
    { source = "versilian"; owner = "sgossner"; name = "VSCO-2-CE"; query = "orchestra"; }
    SECTION ("Versilian piano searches include the community collection")
    { source = "versilian"; owner = "sgossner"; name = "VCSL"; query = "piano"; }
    auto item = repository (42, name, "CC0-1.0");
    item["owner"]["login"] = owner;
    item["description"] = "Sample library";
    auto foreign = item; foreign["id"] = 43; foreign["owner"]["login"] = "someone-else";
    auto restricted = item; restricted["id"] = 44; restricted["license"]["spdx_id"] = "GPL-3.0";
    auto unrelated = item; unrelated["id"] = 45; unrelated["name"] = "Unrelated-project";
    unrelated["description"] = "Unrelated project";
    auto unlicensed = item; unlicensed["id"] = 46; unlicensed["license"]["spdx_id"] = "NOASSERTION";
    FakeTransport transport;
    transport.replies = { { Json::array ({ item, foreign, restricted, unrelated, unlicensed }) } };
    const auto result = browseProvider (transport, source, 1, query);
    REQUIRE (result);
    REQUIRE (result.items.size() == 1);
    CHECK (result.items.front().sourceId == source);
    CHECK (result.items.front().locator == name);
    CHECK (result.items.front().sourceUrl == "https://github.com/" + owner + "/" + name);
    CHECK (transport.requests.front().url.find ("/users/" + owner + "/repos?") != std::string::npos);
    CHECK (transport.requests.size() == 1);
}

TEST_CASE ("Additional GitHub SFZ repositories resolve licensed immutable archives", "[sfz][provider]")
{
    std::string source = "freepats", owner = "freepats", name = "world-percussion";
    SECTION ("FreePats uses the latest source commit") {}
    SECTION ("VSCO uses the SFZ release instead of the raw-sample branch")
    { source = "versilian"; owner = "sgossner"; name = "VSCO-2-CE"; }
    bool readmeLicense = false;
    SECTION ("VCSL uses its generated SFZ release and pinned README declaration")
    { source = "versilian"; owner = "sgossner"; name = "VCSL"; readmeLicense = true; }
    FakeTransport transport;
    const std::string license = readmeLicense
        ? "An open CC0 general-purpose sample library.\nThis collection is under a Creative Commons 0 license.\n"
        : "CC0 1.0 Universal\nhttps://creativecommons.org/publicdomain/zero/1.0/\n";
    transport.replies = githubReplies (license);
    transport.replies[0].json = repository (42, name, "CC0-1.0");
    transport.replies[0].json["owner"]["login"] = owner;
    transport.replies.back().json["license"]["spdx_id"] = "CC0-1.0";
    if (readmeLicense)
    {
        transport.replies[2].json["tree"][0]["path"] = "README.md";
        transport.replies.back().json["path"] = "README.md";
        transport.replies.back().json.erase ("license");
    }
    if (source == "versilian")
        transport.replies.insert (transport.replies.begin() + 1,
            Reply { Json { { "tag_name", "SFZ-v1" }, { "draft", false }, { "prerelease", false } } });
    ProviderSummary summary { source, "42", name, name, owner, {},
        "https://github.com/" + owner + "/" + name, "CC0-1.0" };
    const auto result = resolveProviderPack (transport, summary);
    REQUIRE (result);
    REQUIRE (result.pack);
    const auto& pack = *result.pack;
    const auto prefix = "https://github.com/" + owner + "/" + name;
    CHECK (pack.sourceId == source);
    CHECK (pack.releaseId == kCommit);
    CHECK (pack.downloadUrl == prefix + "/archive/" + kCommit + ".zip");
    CHECK (pack.expectedRoot == name + "-" + kCommit);
    CHECK (pack.license.text == license);
    CHECK (pack.license.id == "CC0-1.0");
    CHECK (pack.license.path == (readmeLicense ? "README.md" : "LICENSE"));
    CHECK (pack.instruments.size() == 2);
    CHECK (pack.versionPinned);
    REQUIRE (providerPackFromJson (providerPackToJson (pack)));
    CHECK (transport.requests[source == "versilian" ? 2 : 1].url.find (
        source == "versilian" ? "/commits/SFZ-v1" : "/commits/HEAD") != std::string::npos);
    auto tampered = Json::parse (providerPackToJson (pack));
    tampered["source_id"] = source == "freepats" ? "versilian" : "freepats";
    CHECK_FALSE (providerPackFromJson (tampered.dump()));
    if (readmeLicense)
        CHECK (transport.requests.back().url.find ("/contents/README.md?ref=" + kCommit) != std::string::npos);
}

TEST_CASE ("Versilian SFZ resolution requires a published release of an approved library", "[sfz][provider]")
{
    FakeTransport transport;
    auto summary = githubSummary();
    summary.sourceId = "versilian";
    summary.locator = "VCSL";
    auto repo = repository (42, "VCSL", "CC0-1.0");
    repo["owner"]["login"] = "sgossner";
    Json release { { "tag_name", "SFZ-v1" }, { "draft", false }, { "prerelease", false } };
    SECTION ("An unrelated repository is not accepted") { summary.locator = "Unrelated-project"; }
    SECTION ("A missing tag is not HEAD") { release.erase ("tag_name"); }
    SECTION ("A draft is not a published release") { release["draft"] = true; }
    SECTION ("A prerelease is not selected") { release["prerelease"] = true; }
    transport.replies = { { repo }, { release } };
    CHECK_FALSE (resolveProviderPack (transport, summary));
    CHECK (transport.requests.size() <= 2);
}

TEST_CASE ("SFZ Instruments browsing filters known non-CC licenses and retains pagination", "[sfz][provider]")
{
    FakeTransport transport;
    auto item = repository();
    std::optional<std::uint64_t> stars;
    SECTION ("Missing popularity stays unknown") {}
    SECTION ("Zero is a real count") { item["stargazers_count"] = 0; stars = 0; }
    SECTION ("Source stars are retained") { item["stargazers_count"] = 17; stars = 17; }
    SECTION ("Negative counts are ignored") { item["stargazers_count"] = -1; }
    SECTION ("Fractional counts are ignored") { item["stargazers_count"] = 1.5; }
    SECTION ("String counts are ignored") { item["stargazers_count"] = "17"; }
    transport.replies = { { Json::array ({ item, repository (43, "Synth", "GPL-3.0"),
                                           repository (44, "Unknown", "NOASSERTION") }), 200,
                           { { "link", "<https://api.github.com/orgs/sfzinstruments/repos?per_page=50&page=2>; rel=\"next\"" },
                             { "x-ratelimit-remaining", "57" } } } };
    const auto result = browseProvider (transport, "sfzinstruments");
    REQUIRE (result);
    REQUIRE (result.items.size() == 2);
    CHECK (result.items[0].packId == "42");
    CHECK (result.items[0].presentation.stars == stars);
    CHECK_FALSE (result.items[0].presentation.favorites);
    CHECK (result.items[1].licenseHint == "NOASSERTION");
    CHECK (result.nextPage == 2);
    CHECK (result.rateLimit.remaining == "57");
    CHECK (transport.requests.front().headers.at ("accept") == "application/vnd.github+json");
    CHECK (transport.requests.size() == 1);
}

TEST_CASE ("SFZ provider browse queries preserve bounded API requests", "[sfz][provider]")
{
    FakeTransport transport;
    SECTION ("GitHub query is a local summary filter")
    {
        auto synth = repository (43, "Synth");
        synth["description"] = "A synthesizer collection";
        Reply optional { {}, 403 };
        SECTION ("Unavailable catalog keeps ordinary GitHub matches") {}
        SECTION ("Transport exceptions keep ordinary GitHub matches")
        { optional = { {}, 200, {}, "catalog", [] { throw std::runtime_error ("Fixture connection failure"); } }; }
        SECTION ("Oversized catalog declaration keeps ordinary GitHub matches")
        { optional = { {}, 200, { { "content-length", "131073" } } }; }
        transport.replies = { { Json::array ({ repository(), synth }) }, optional };
        const auto result = browseProvider (transport, "sfzinstruments", 1, "piano");
        REQUIRE (result);
        REQUIRE (result.items.size() == 1);
        CHECK (result.items.front().packId == "42");
        REQUIRE (transport.requests.size() == 2);
        CHECK (transport.requests.front().url == "https://api.github.com/orgs/sfzinstruments/repos?type=public&sort=full_name&per_page=50&page=1");
        CHECK (transport.requests.back().url == "https://raw.githubusercontent.com/sfzinstruments/sfzinstruments.github.io/master/data/sfz/instruments.yml");
        CHECK (transport.requests.back().timeoutSeconds == 5);
    }
    SECTION ("SFZ Instruments matches official descriptions before filtering")
    {
        const std::string description = "Punk/indie drums and unusual percussion played with brushes.";
        const std::string header = "- name: Drums\n  page: drums\n  instruments:\n"
            "  - name: Swirly Drums\n  # page: swirly_drums\n    license: CC0-1.0\n"
            "    url: https://karoryfer-samples.myshopify.com/pages/free-swirly-drums\n";
        std::string text = "    short_description:\n      \"" + description + "\"\n";
        std::string suffix, expectedDescription = description, query = "BrUsHeS";
        bool matches = true;
        auto swirly = repository (42, "karoryfer.swirly-drums", "NOASSERTION");
        swirly["description"] = nullptr;
        auto other = repository (43, "Unrelated");
        other["description"] = nullptr;
        SECTION ("Real Swirly Drums has a null GitHub description") {}
        SECTION ("Quoted continuation is folded with spaces")
        { text = "    short_description: \"Punk/indie drums and unusual percussion played with\n      brushes.\"\n"; query = "with brushes"; }
        SECTION ("Folded YAML text is searchable")
        { text = "    short_description: >- # excerpt\n      Punk/indie drums and unusual percussion played with\n      brushes.\n"; query = "with brushes"; }
        SECTION ("Plain continuation is searchable")
        { text = "    short_description: Punk/indie drums and unusual percussion played with\n      brushes.\n"; query = "with brushes"; }
        SECTION ("Single-quoted continuation preserves escaped apostrophes")
        {
            text = "    short_description: 'Drummer''s kit played with\n      brushes.'\n";
            expectedDescription = "Drummer's kit played with brushes."; query = "with brushes";
        }
        SECTION ("GitHub descriptions are retained alongside catalog text")
        { swirly["description"] = "Original description"; expectedDescription = "Original description " + description; }
        SECTION ("Explicit repository URLs are matched")
        { suffix = "    downloads:\n    - label: Instrument\n      url: https://github.com/sfzinstruments/karoryfer.swirly-drums/releases/download/v1/pack.zip\n"; }
        SECTION ("Download descriptions do not replace the instrument description")
        { suffix = "    downloads:\n    - label: Samples\n      short_description: Unrelated sample download\n"; }
        SECTION ("Different explicit repository prevents a title guess")
        { suffix = "    downloads:\n    - label: Instrument\n      url: https://github.com/sfzinstruments/someone.swirly-drums\n"; matches = false; }
        SECTION ("Descriptions do not spill into another entry")
        { text.clear(); suffix = "  - name: Other Drums\n    page: other\n    short_description: brushes\n"; matches = false; }
        SECTION ("Duplicate description fields are ambiguous")
        { suffix = "    short_description: brushes\n"; matches = false; }
        SECTION ("Multiple matched entries are ambiguous even with the same page")
        { suffix = "  - name: Swirly Drums\n    short_description: brushes\n"; matches = false; }
        SECTION ("Unterminated quoted descriptions are ignored")
        { text = "    short_description: \"brushes\n"; matches = false; }
        SECTION ("YAML aliases are not interpreted")
        { text = "    short_description: *brushes\n"; matches = false; }
        SECTION ("Oversized continued descriptions are ignored")
        { text = "    short_description: >\n      " + std::string (1100, 'a') + "\n      " + std::string (1100, 'b') + " brushes\n"; matches = false; }
        SECTION ("Catalog lines remain bounded")
        { suffix = "#" + std::string (2048, 'x') + "\n"; matches = false; }
        transport.replies = { { Json::array ({ swirly, other }), 200,
            { { "link", "<next>; rel=\"next\"" }, { "x-ratelimit-remaining", "57" } } },
            { {}, 200, {}, header + text + suffix } };
        const auto result = browseProvider (transport, "sfzinstruments", 1, query);
        CAPTURE (query, text, suffix);
        REQUIRE (result);
        REQUIRE (result.items.size() == (matches ? 1 : 0));
        if (matches)
        {
            CHECK (result.items.front().locator == "karoryfer.swirly-drums");
            CHECK (result.items.front().description == expectedDescription);
            CHECK (result.items.front().presentation.categoryHint == expectedDescription);
            CHECK (result.items.front().licenseHint == "NOASSERTION");
        }
        CHECK (result.nextPage == 2);
        CHECK (result.rateLimit.remaining == "57");
        CHECK (transport.requests.size() == 2);
    }
    SECTION ("Musical Artifacts query is encoded")
    {
        transport.replies = { { Json::array ({ artifact() }), 200, { { "x-total-pages", "3" } } } };
        const auto result = browseProvider (transport, "musical-artifacts", 1, "piano & bells");
        REQUIRE (result);
        CHECK (result.nextPage == 2);
        CHECK (transport.requests.front().url.find ("q=piano%20%26%20bells") != std::string::npos);
        CHECK (transport.requests.size() == 1);
    }
    SECTION ("Unbounded page is rejected before transport")
    {
        CHECK_FALSE (browseProvider (transport, "sfzinstruments", 201));
        CHECK (transport.requests.empty());
    }
    SECTION ("Unbounded query is rejected before transport")
    {
        CHECK_FALSE (browseProvider (transport, "sfzinstruments", 1, std::string (257, 'a')));
        CHECK (transport.requests.empty());
    }
    SECTION ("Maximum page and query remain valid")
    {
        transport.replies = { { Json::array() }, { {}, 403 } };
        const auto result = browseProvider (transport, "sfzinstruments", 200, std::string (256, 'a'));
        REQUIRE (result);
        CHECK (result.nextPage == 0);
        REQUIRE (transport.requests.size() == 2);
        CHECK (transport.requests.front().url.find ("per_page=50&page=200") != std::string::npos);
    }
}

TEST_CASE ("SFZ Instruments resolves pinned archives with actual LICENSE bytes", "[sfz][provider]")
{
    FakeTransport transport;
    transport.replies = githubReplies();
    transport.replies[0].json["stargazers_count"] = 7;
    const std::string catalog = "- name: \"Woodwinds\"\n  page: \"woodwinds\"\n  instruments:\n"
        "  - name: \"Grand Piano\"\n    page: \"grand_piano\"\n"
        "    url: \"https://github.com/sfzinstruments/Grand-Piano\"\n";
    std::string projection = catalog;
    std::string html = "<audio src=\"/assets/audio/woodwinds/grand_piano.mp3\" controls></audio>";
    bool demo = false;
    int optionalStatus = 200;
    SECTION ("Explicit catalog match and published audio") { demo = true; }
    SECTION ("Matching download repository")
    {
        projection = "- name: \"Woodwinds\"\n  page: \"woodwinds\"\n  instruments:\n"
            "  - name: \"Grand Piano\"\n    page: \"grand_piano\"\n    downloads:\n"
            "    - label: \"Instrument\"\n      url: \"https://github.com/sfzinstruments/Grand-Piano/releases/download/v1/pack.zip\"\n";
        demo = true;
    }
    SECTION ("Different repository is not guessed from its title")
    { projection.replace (projection.find ("/Grand-Piano"), 12, "/Other-Piano"); }
    SECTION ("Conflicting catalog pages are ambiguous")
    { projection += "  - name: \"Other version\"\n    page: \"other\"\n    url: \"https://github.com/sfzinstruments/Grand-Piano\"\n"; }
    SECTION ("Duplicate page property is ambiguous") { projection += "    page: \"other\"\n"; }
    SECTION ("No audio element") { html = "<p>No preview published.</p>"; }
    SECTION ("Commented audio is not published") { html = "<!--" + html + "-->"; }
    SECTION ("Different audio asset is not guessed") { html = "<audio src=\"/assets/audio/other.mp3\"></audio>"; }
    SECTION ("Unsafe catalog page is ignored") { projection.replace (projection.find ("grand_piano"), 11, "../outside"); }
    SECTION ("Non-HTTPS mapping is ignored") { projection.replace (projection.find ("https:"), 6, "http:"); }
    SECTION ("Optional metadata denial does not invalidate license") { optionalStatus = 403; }
    SECTION ("Optional transport exception does not invalidate license")
    {
        transport.replies.push_back ({ {}, 200, {}, "catalog", [] { throw std::runtime_error ("Fixture connection failure"); } });
    }
    transport.replies.push_back ({ {}, optionalStatus, {}, projection });
    transport.replies.push_back ({ {}, 200, {}, html });
    const auto result = resolveProviderPack (transport, githubSummary());
    REQUIRE (result);
    REQUIRE (result.pack);
    const auto& pack = *result.pack;
    CHECK (pack.releaseId == kCommit);
    CHECK (pack.versionPinned);
    CHECK (pack.expectedSha256.empty());
    CHECK (pack.expectedRoot == "Grand-Piano-" + kCommit);
    CHECK (pack.downloadUrl == "https://github.com/sfzinstruments/Grand-Piano/archive/" + kCommit + ".zip");
    CHECK (pack.license.text == kLicenseText);
    CHECK (pack.license.path == "LICENSE");
    CHECK (pack.license.sha256.size() == 64);
    CHECK (pack.presentation.stars == 7);
    CHECK (pack.presentation.demos.size() == (demo ? 1 : 0));
    if (demo)
    {
        CHECK (pack.presentation.demos.front().url == "https://sfzinstruments.github.io/woodwinds/grand_piano/");
        CHECK (transport.requests.back().timeoutSeconds == 5);
    }
    REQUIRE (pack.instruments.size() == 2);
    CHECK (pack.instruments.front().candidateOnly);
    CHECK (pack.instruments.back().relativePath == "Instruments/Piano.sfz");
    CHECK (transport.requests[3].url.find ("ref=" + kCommit) != std::string::npos);
    REQUIRE (providerPackFromJson (providerPackToJson (pack)));
}

TEST_CASE ("SFZ Instruments does not accept SPDX hints without actual matching licenses", "[sfz][provider][issue-903]")
{
    FakeTransport transport;
    bool combinedLimit = false;
    const auto secondaryLicense = [&] (const std::string& text, const std::string& path = "Samples/LICENSE.txt")
    {
        transport.replies = githubReplies();
        transport.replies[2].json["tree"].push_back (
            { { "path", path }, { "type", "blob" }, { "mode", "100644" } });
        transport.replies.push_back ({ { { "path", path }, { "encoding", "base64" }, { "content", base64 (text) } } });
    };
    SECTION ("Samples have a restricted secondary license")
    { secondaryLicense ("https://creativecommons.org/licenses/by-nc/4.0/"); }
    SECTION ("A sidecar declares NoDerivatives")
    { secondaryLicense ("https://creativecommons.org/licenses/by-nd/4.0/", "Samples/a.wav.license"); }
    SECTION ("A sidecar declares spaced NoDerivatives")
    { secondaryLicense (kLicenseText + "except the choir, which is CC BY-ND 4.0", "Samples/a.wav.license"); }
    SECTION ("A sidecar declares spaced NonCommercial ShareAlike")
    { secondaryLicense (kLicenseText + "except the organ, CC BY-NC-SA 4.0"); }
    SECTION ("A sidecar prohibits commercial use in prose")
    { secondaryLicense (kLicenseText + "Commercial use is not permitted."); }
    SECTION ("A sidecar declares a bare hyphenated NonCommercial element")
    { secondaryLicense (kLicenseText + "Samples: BY-NC."); }
    SECTION ("A sidecar declares a bare NoDerivatives element with an em dash")
    { secondaryLicense (kLicenseText + "Drums: BY\xe2\x80\x94ND."); }
    SECTION ("A differently named license document conflicts")
    { secondaryLicense ("https://creativecommons.org/licenses/by-sa/4.0/", "Samples/LICENCE-MAPPINGS"); }
    SECTION ("A document in a licensing directory is unestablished")
    { secondaryLicense ("Ask the author for permission.", "LICENSES/samples.txt"); }
    SECTION ("Licensing symlinks are not skipped")
    { secondaryLicense (kLicenseText); transport.replies[2].json["tree"].back()["mode"] = "120000"; }
    SECTION ("Portable license aliases are ambiguous")
    {
        secondaryLicense (kLicenseText, "license");
        transport.replies[2].json["tree"].push_back (
            { { "path", "LICENSE" }, { "type", "blob" }, { "mode", "100644" } });
    }
    SECTION ("Secondary document responses must name the exact path")
    { secondaryLicense (kLicenseText); transport.replies.back().json["path"] = "Other/LICENSE.txt"; }
    SECTION ("Oversized secondary documents are refused")
    { secondaryLicense (kLicenseText + std::string (kMaxLicenseBytes, ' ')); }
    SECTION ("Too many licensing documents are refused")
    {
        secondaryLicense (kLicenseText);
        for (std::size_t i = 0; i < kMaxLicenseDocuments; ++i)
            transport.replies[2].json["tree"].push_back (
                { { "path", "Samples/LICENSE-" + std::to_string (i) }, { "type", "blob" }, { "mode", "100644" } });
    }
    SECTION ("Combined licensing bytes are bounded")
    {
        combinedLimit = true;
        transport.replies = githubReplies();
        const auto text = kLicenseText + std::string (kMaxLicenseBytes - kLicenseText.size(), ' ');
        for (int i = 0; i < 11; ++i)
        {
            const auto path = "Samples/LICENSE-" + std::to_string (10 + i);
            transport.replies[2].json["tree"].push_back (
                { { "path", path }, { "type", "blob" }, { "mode", "100644" } });
            transport.replies.push_back ({ { { "path", path }, { "encoding", "base64" }, { "content", base64 (text) } } });
        }
    }
    SECTION ("Only hint is present") { transport.replies = githubReplies ("All rights reserved; available for free."); }
    SECTION ("Generic public domain is not CC0") { transport.replies = githubReplies ("This instrument is public domain."); }
    SECTION ("A CC0 project description without the grant is not a license")
    { transport.replies = githubReplies ("An open CC0 general-purpose sample library."); }
    SECTION ("An unversioned grant without the explicit CC0 identity is not guessed")
    { transport.replies = githubReplies ("This collection is under a Creative Commons 0 license."); }
    SECTION ("Different CC license than API hint")
    { transport.replies = githubReplies ("https://creativecommons.org/licenses/by-nc/4.0/"); }
    SECTION ("Ambiguous CC licenses")
    { transport.replies = githubReplies (kLicenseText + "\nhttps://creativecommons.org/licenses/by-sa/4.0/"); }
    SECTION ("An SPDX identifier alone is not a grant")
    { transport.replies = githubReplies ("SPDX-License-Identifier: CC-BY-4.0"); }
    SECTION ("Plain commercial restrictions conflict with the grant")
    { transport.replies = githubReplies (kLicenseText + "\nFor non-commercial\nuse only."); }
    SECTION ("Plain modification restrictions conflict with the grant")
    { transport.replies = githubReplies (kLicenseText + "\nNo derivative works are permitted."); }
    SECTION ("A spaced NoDerivatives identifier conflicts with the primary grant")
    { transport.replies = githubReplies (kLicenseText + "except the choir, which is CC BY-ND 4.0"); }
    SECTION ("A spaced NonCommercial ShareAlike identifier conflicts with the primary grant")
    { transport.replies = githubReplies (kLicenseText + "except the organ, CC BY-NC-SA 4.0"); }
    SECTION ("A spelled-out Creative Commons prefix still declares restricted terms")
    { transport.replies = githubReplies (kLicenseText + "Audio recordings use Creative Commons BY-ND 4.0."); }
    SECTION ("A spaced and wrapped restricted identifier conflicts with the primary grant")
    { transport.replies = githubReplies (kLicenseText + "CC\tBY -\nND 4.0"); }
    SECTION ("Markdown formatting does not hide a restricted identifier")
    { transport.replies = githubReplies (kLicenseText + "CC **BY-NC** 4.0"); }
    SECTION ("A bare versioned NonCommercial identifier conflicts with the primary grant")
    { transport.replies = githubReplies (kLicenseText + "The choir recordings are BY-NC 4.0."); }
    SECTION ("A bare NonCommercial ShareAlike identifier conflicts with the primary grant")
    { transport.replies = githubReplies (kLicenseText + "The organ samples are BY-NC-SA."); }
    SECTION ("A bare versioned NoDerivatives identifier conflicts with the primary grant")
    { transport.replies = githubReplies (kLicenseText + "Drums: BY-ND 3.0"); }
    SECTION ("A nonbreaking hyphen does not hide a restricted identifier")
    { transport.replies = githubReplies (kLicenseText + "CC BY\xe2\x80\x91ND 4.0"); }
    SECTION ("Commercial use is explicitly prohibited despite the primary grant")
    { transport.replies = githubReplies (kLicenseText + "Commercial use is not permitted."); }
    SECTION ("A separate restricted SPDX declaration conflicts with CC0")
    {
        transport.replies = githubReplies ("Mapping files use CC0 1.0 Universal.\nAudio recordings use CC-BY-NC-4.0.");
        transport.replies.back().json["license"]["spdx_id"] = "CC0-1.0";
    }
    SECTION ("A separate NoDerivatives SPDX declaration conflicts with BY")
    { transport.replies = githubReplies (kLicenseText + "\nAudio recordings use CC-BY-ND-4.0."); }
    SECTION ("A wrapped NonCommercial SPDX declaration conflicts with BY")
    { transport.replies = githubReplies (kLicenseText + "\nAudio recordings use CC-\nBY-NC-4.0."); }
    SECTION ("A wrapped NoDerivatives SPDX declaration conflicts with BY")
    { transport.replies = githubReplies (kLicenseText + "\nAudio recordings use CC-BY-\nND-4.0."); }
    SECTION ("An unknown SPDX declaration is not ignored")
    { transport.replies = githubReplies (kLicenseText + "\nAudio recordings use CC-BY-9.0."); }
    SECTION ("CC grant does not override a reservation for the samples")
    {
        transport.replies = githubReplies ("Scripts: CC0 1.0 Universal. Samples: all rights reserved.");
        transport.replies.back().json["license"]["spdx_id"] = "CC0-1.0";
    }
    SECTION ("A grant scoped only to scripts does not license the instrument pack")
    {
        transport.replies = githubReplies ("**Scripts:** CC0 1.0 Universal.\nSamples have a separate license.");
        transport.replies.back().json["license"]["spdx_id"] = "CC0-1.0";
    }
    SECTION ("A grant scoped only to code is not a pack license")
    { transport.replies = githubReplies ("Code: " + kLicenseText); }
    SECTION ("An exception to the grant needs manual licensing review")
    { transport.replies = githubReplies (kLicenseText + "\nThe samples are not covered by this license."); }
    const auto result = resolveProviderPack (transport, githubSummary());
    CHECK_FALSE (result);
    if (combinedLimit) CHECK (result.error.find ("combined size limit") != std::string::npos);
}

TEST_CASE ("SFZ Instruments accepts formal Creative Commons license text", "[sfz][provider][issue-903]")
{
    FakeTransport transport;
    const auto completeLicense = [&] (const char* file, const char* id)
    {
        const auto text = dusk::fs::loadFileAsString (
            std::filesystem::u8path (DUSKSTUDIO_SFZ_LICENSE_FIXTURES) / file);
        REQUIRE (text.size() > 10000);
        transport.replies = githubReplies (text);
        transport.replies.back().json["license"]["spdx_id"] = id;
    };
    SECTION ("Complete CC BY 4.0 legal code") { completeLicense ("cc-by-4.0.txt", "CC-BY-4.0"); }
    SECTION ("Complete CC BY-SA 3.0 legal code") { completeLicense ("cc-by-sa-3.0.txt", "CC-BY-SA-3.0"); }
    SECTION ("Formal attribution title")
    { transport.replies = githubReplies ("Creative Commons Attribution 4.0 International Public License\nTerms follow.\n"); }
    SECTION ("A matching SPDX identifier agrees with the grant")
    { transport.replies = githubReplies (kLicenseText + "\nSPDX-License-Identifier: CC-BY-4.0."); }
    SECTION ("An explicit commercial permission remains allowed")
    { transport.replies = githubReplies (kLicenseText + "Commercial use is not restricted."); }
    SECTION ("An author name is not a restricted license declaration")
    { transport.replies = githubReplies (kLicenseText + "Produced by NCK Audio."); }
    SECTION ("Author initials matching license abbreviations remain allowed")
    { transport.replies = githubReplies (kLicenseText + "Produced by NC Audio. Artwork by ND Studio."); }
    SECTION ("A matching sample-specific secondary grant is retained")
    {
        transport.replies = githubReplies();
        transport.replies[2].json["tree"].push_back (
            { { "path", "Samples/LICENCE.txt" }, { "type", "blob" }, { "mode", "100644" } });
        transport.replies.push_back ({ { { "path", "Samples/LICENCE.txt" }, { "encoding", "base64" },
            { "content", base64 ("Samples: " + kLicenseText) } } });
    }
    SECTION ("Nested licensing directories contain matching documents")
    {
        transport.replies = githubReplies();
        for (const auto* path : { "LICENSES", "LICENSES/nested" })
            transport.replies[2].json["tree"].push_back (
                { { "path", path }, { "type", "tree" }, { "mode", "040000" } });
        transport.replies[2].json["tree"].push_back (
            { { "path", "LICENSES/nested/audio.txt" }, { "type", "blob" }, { "mode", "100644" } });
        transport.replies.push_back ({ { { "path", "LICENSES/nested/audio.txt" }, { "encoding", "base64" },
            { "content", base64 (kLicenseText) } } });
    }
    SECTION ("Explicit CC0 dedication")
    {
        transport.replies = githubReplies ("CC0 1.0 Universal\nhttps://creativecommons.org/publicdomain/zero/1.0/\n");
        transport.replies.back().json["license"]["spdx_id"] = "CC0-1.0";
    }
    REQUIRE (resolveProviderPack (transport, githubSummary()));
}

TEST_CASE ("SFZ Instruments rejects incomplete trees and unsafe candidate paths", "[sfz][provider]")
{
    FakeTransport transport;
    transport.replies = githubReplies();
    SECTION ("Tree is truncated") { transport.replies[2].json["truncated"] = true; }
    SECTION ("Traversal path") { transport.replies[2].json["tree"][1]["path"] = "../Piano.sfz"; }
    SECTION ("Case alias")
    { transport.replies[2].json["tree"].push_back ({ { "path", "instruments/piano.sfz" }, { "mode", "100644" }, { "type", "blob" } }); }
    SECTION ("Repository identity changed") { transport.replies[0].json["id"] = 99; }
    SECTION ("License file missing from pinned tree") { transport.replies[3].json["path"] = "OTHER-LICENSE"; }
    CHECK_FALSE (resolveProviderPack (transport, githubSummary()));
}

TEST_CASE ("Musical Artifacts browsing excludes generic free and gray-area items", "[sfz][provider]")
{
    FakeTransport transport;
    auto item = artifact();
    std::optional<std::uint64_t> favorites;
    SECTION ("Missing favorites stay unknown") {}
    SECTION ("Zero favorites are retained") { item["favorite_count"] = 0; favorites = 0; }
    SECTION ("Source favorites are retained") { item["favorite_count"] = 23; favorites = 23; }
    SECTION ("Negative favorites are ignored") { item["favorite_count"] = -2; }
    SECTION ("Fractional favorites are ignored") { item["favorite_count"] = 3.5; }
    transport.replies = { { Json::array ({ item, artifact (78, "public"), artifact (79, "gray"),
                                           artifact (80, "gpl-v3"), artifact (81, "by-nc-nd") }) } };
    const auto result = browseProvider (transport, "musical-artifacts");
    REQUIRE (result);
    REQUIRE (result.items.size() == 1);
    CHECK (result.items.front().presentation.favorites == favorites);
    CHECK_FALSE (result.items.front().presentation.stars);
    CHECK (result.items.front().licenseHint == "CC-BY-4.0");
}

TEST_CASE ("Musical Artifacts resolves declared CC packs and retains upstream SHA-256", "[sfz][provider]")
{
    FakeTransport transport;
    auto item = artifact();
    std::size_t demos = 0;
    item["favorite_count"] = 0;
    SECTION ("No optional demos") {}
    SECTION ("Known media and direct audio links")
    {
        item["more_info"] = Json::array ({ "https://www.youtube.com/watch?v=demo", "https://vimeo.com/12345",
            "https://soundcloud.com/artist/track", "https://example.org/audio.mp3", "https://example.org/audio.mp3" });
        demos = 4;
    }
    SECTION ("Ordinary links and unsafe URLs are not demos")
    {
        item["more_info"] = Json::array ({ "https://example.org/about", "http://youtu.be/demo", "javascript:alert(1)",
            "https://youtube.com.evil.invalid/watch?v=demo", "https://user@youtube.com/watch?v=demo", "https://www.youtube.com/channel/demo" });
    }
    SECTION ("Unbounded optional link list is ignored")
    { item["more_info"] = std::vector<std::string> (65, "https://youtu.be/demo"); }
    transport.replies = { { item }, { artifactLicenses() } };
    const auto result = resolveProviderPack (transport, artifactSummary());
    REQUIRE (result);
    REQUIRE (result.pack);
    const auto& pack = *result.pack;
    CHECK (pack.expectedSha256 == kArchiveHash);
    CHECK (pack.releaseId == kArchiveHash);
    CHECK (pack.advertisedBytes == 1024);
    CHECK (pack.license.id == "CC-BY-4.0");
    CHECK (pack.license.path.empty());
    CHECK (pack.presentation.favorites == 0);
    CHECK (pack.presentation.demos.size() == demos);
    CHECK (pack.license.text.find ("Creative Commons Attribution 4.0") != std::string::npos);
    REQUIRE (pack.instruments.size() == 1);
    CHECK (pack.instruments.front().relativePath == "Piano/Piano.sfz");
    REQUIRE (providerPackFromJson (providerPackToJson (pack)));
}

TEST_CASE ("Musical Artifacts preserves license constraints and version distinctions", "[sfz][provider]")
{
    for (const auto& license : kLicenseExamples)
    {
        CAPTURE (license.id);
        FakeTransport transport;
        transport.replies = { { artifact (77, license.artifactName) },
                              { artifactLicenses (license.artifactName, license.title) } };
        const auto result = resolveProviderPack (transport, artifactSummary());
        REQUIRE (static_cast<bool> (result) == license.allowed);
        if (license.allowed)
        {
            REQUIRE (result.pack);
            CHECK (result.pack->license.id == license.id);
            if (std::string_view (license.id).find ("BY") != std::string_view::npos)
                CHECK (result.pack->license.constraints.find ("Attribution") != std::string::npos);
            if (std::string_view (license.id).find ("SA") != std::string_view::npos)
                CHECK (result.pack->license.constraints.find ("ShareAlike") != std::string::npos);
            CHECK (providerPackFromJson (providerPackToJson (*result.pack)));
        }
        else
        {
            CHECK_FALSE (result.pack);
            CHECK (result.error.find ("NonCommercial and NoDerivatives") != std::string::npos);
        }
        FakeTransport listing;
        listing.replies = { { Json::array ({ artifact (77, license.artifactName) }) } };
        const auto page = browseProvider (listing, "musical-artifacts");
        REQUIRE (page);
        CHECK (page.items.size() == (license.allowed ? 1 : 0));
    }
}

TEST_CASE ("GitHub SFZ sources refuse restricted licenses in listings and resolved packs", "[sfz][provider]")
{
    for (const std::string source : { "sfzinstruments", "freepats", "versilian" })
        for (const auto& license : kLicenseExamples)
        {
            CAPTURE (source, license.id);
            const std::string owner = source == "versilian" ? "sgossner" : source;
            const std::string name = source == "versilian" ? "VSCO-2-CE" : "Grand-Piano";
            auto item = repository (42, name, license.id);
            item["owner"]["login"] = owner;
            FakeTransport listing;
            listing.replies = { { Json::array ({ item }) } };
            const auto page = browseProvider (listing, source);
            REQUIRE (page);
            CHECK (page.items.size() == (license.allowed ? 1 : 0));

            FakeTransport transport;
            transport.replies = githubReplies (license.title);
            transport.replies.front().json = item;
            transport.replies.back().json["license"]["spdx_id"] = license.id;
            if (source == "versilian")
                transport.replies.insert (transport.replies.begin() + 1,
                    Reply { Json { { "tag_name", "SFZ-v1" }, { "draft", false }, { "prerelease", false } } });
            ProviderSummary summary { source, "42", name, name, owner, {},
                "https://github.com/" + owner + "/" + name, "NOASSERTION" };
            const auto result = resolveProviderPack (transport, summary);
            REQUIRE (static_cast<bool> (result) == license.allowed);
            if (license.allowed)
            {
                REQUIRE (result.pack);
                CHECK (result.pack->license.id == license.id);
                CHECK (providerPackFromJson (providerPackToJson (*result.pack)));
                for (const auto& restricted : kLicenseExamples)
                    if (! restricted.allowed)
                    {
                        auto cached = *result.pack;
                        cached.license.id = restricted.id;
                        const auto refused = providerPackFromJson (providerPackToJson (cached));
                        CHECK_FALSE (refused);
                        CHECK (refused.error.find ("NonCommercial and NoDerivatives") != std::string::npos);
                    }
            }
            else
            {
                CHECK_FALSE (result.pack);
                CHECK (result.error.find ("NonCommercial and NoDerivatives") != std::string::npos);
            }
        }
}

TEST_CASE ("Musical Artifacts does not invent missing checksums or archive entrypoints", "[sfz][provider]")
{
    FakeTransport transport;
    auto item = artifact();
    item.erase ("file_hash");
    item.erase ("file_list");
    transport.replies = { { item }, { artifactLicenses() } };
    const auto result = resolveProviderPack (transport, artifactSummary());
    REQUIRE (result);
    CHECK (result.pack->expectedSha256.empty());
    CHECK (result.pack->releaseId == "file-99");
    CHECK_FALSE (result.pack->versionPinned);
    CHECK (result.pack->instruments.empty());
    REQUIRE (providerPackFromJson (providerPackToJson (*result.pack)));
}

TEST_CASE ("Musical Artifacts rejects inconsistent declarations and download metadata", "[sfz][provider]")
{
    FakeTransport transport;
    auto item = artifact(), licenses = artifactLicenses();
    SECTION ("Unrecognized license") { item["license"] = "gray"; }
    SECTION ("License definition has a different variant") { licenses[0]["name"] = "Creative Commons Attribution-NonCommercial 4.0"; }
    SECTION ("License definition adds a commercial restriction")
    { licenses[0]["name"] = "Creative Commons Attribution 4.0. For non-commercial use only."; }
    SECTION ("License definition adds a restricted SPDX identifier")
    { licenses[0]["name"] = "Creative Commons Attribution 4.0. Samples: CC-BY-ND-4.0."; }
    SECTION ("Conflicting duplicate license definitions")
    {
        licenses.push_back (licenses[0]);
        licenses[1]["name"] = "Creative Commons Attribution-NonCommercial 4.0";
    }
    SECTION ("Unsafe declared SFZ path") { item["file_list"] = Json::array ({ "../Piano.sfz" }); }
    SECTION ("Unsupported archive") { item["file"] = "https://musical-artifacts.com/artifacts/77/piano.7z"; }
    SECTION ("HTTP download") { item["file"] = "http://musical-artifacts.com/artifacts/77/piano.zip"; }
    SECTION ("Malformed advertised hash") { item["file_hash"] = "not a sha256"; }
    SECTION ("Mismatching artifact identity") { item["id"] = 88; }
    transport.replies = { { item }, { licenses } };
    CHECK_FALSE (resolveProviderPack (transport, artifactSummary()));
}

TEST_CASE ("SFZ providers report rate limits and browser challenges without bypassing them", "[sfz][provider]")
{
    FakeTransport transport;
    SECTION ("GitHub rate-limit response")
    {
        transport.replies = { { { { "message", "API rate limit exceeded" } }, 403,
                               { { "x-ratelimit-remaining", "0" }, { "x-ratelimit-reset", "2000000000" } } } };
        const auto result = browseProvider (transport, "sfzinstruments");
        CHECK_FALSE (result);
        CHECK (result.rateLimit.remaining == "0");
        CHECK (result.error.find ("rate limit") != std::string::npos);
    }
    SECTION ("Retry-After response")
    {
        transport.replies = { { {}, 429, { { "retry-after", "120" } } } };
        const auto result = browseProvider (transport, "musical-artifacts");
        CHECK_FALSE (result);
        CHECK (result.rateLimit.retryAfter == "120");
    }
    SECTION ("HTML anti-bot response")
    {
        transport.replies = { { {}, 403, { { "content-type", "text/html" } }, "<html>Just a moment...</html>" } };
        const auto result = browseProvider (transport, "musical-artifacts");
        CHECK_FALSE (result);
        CHECK (result.error.find ("browser or anti-bot") != std::string::npos);
    }
    CHECK (transport.requests.size() == 1);
}

TEST_CASE ("SFZ providers reject malformed or oversized API responses", "[sfz][provider]")
{
    FakeTransport transport;
    SECTION ("Duplicate JSON keys")
    { transport.replies = { { {}, 200, {}, "[{\"id\":42,\"id\":43}]" } }; }
    SECTION ("Unexpected response shape") { transport.replies = { { Json::object() } }; }
    SECTION ("Oversized body declaration")
    { transport.replies = { { Json::array(), 200, { { "content-length", "999999999" } } } }; }
    CHECK_FALSE (browseProvider (transport, "sfzinstruments"));
}

TEST_CASE ("SFZ cached descriptors validate paths, pins, and license receipts offline", "[sfz][provider]")
{
    const auto pack = resolvedGithub();
    auto cached = Json::parse (providerPackToJson (pack));
    SECTION ("Valid descriptor roundtrips")
    {
        const auto result = providerPackFromJson (cached.dump());
        REQUIRE (result);
        CHECK (result.pack->releaseId == pack.releaseId);
        CHECK (result.pack->instruments.front().id == pack.instruments.front().id);
        return;
    }
    SECTION ("Presentation does not change receipt or resume identity")
    {
        auto decorated = pack;
        decorated.presentation.stars = 99;
        decorated.presentation.favorites = 2;
        decorated.presentation.demos.push_back ({ "Source demo", "https://example.org/demo.mp3" });
        decorated.presentation.artworkUrl = "https://example.org/piano.jpg";
        decorated.presentation.categoryHint = "piano";
        CHECK (providerPackToJson (decorated) == providerPackToJson (pack));
        cached["presentation"] = { { "stars", 999 }, { "demos", Json::array ({ "https://example.org/demo.mp3" }) } };
        const auto result = providerPackFromJson (cached.dump());
        REQUIRE (result);
        CHECK_FALSE (result.pack->presentation.stars);
        CHECK_FALSE (result.pack->presentation.favorites);
        CHECK (result.pack->presentation.demos.empty());
        return;
    }
    SECTION ("Installed receipts retain licensing admitted before stricter download rules")
    {
        auto installed = pack;
        SECTION ("GitHub") {}
        SECTION ("Musical Artifacts")
        {
            FakeTransport transport;
            transport.replies = { { artifact() }, { artifactLicenses() } };
            const auto result = resolveProviderPack (transport, artifactSummary());
            REQUIRE (result);
            installed = *result.pack;
        }
        installed.license.text += "\nSamples have a separate license.\n";
        REQUIRE (sodium_init() >= 0);
        std::array<unsigned char, crypto_hash_sha256_BYTES> hash {};
        crypto_hash_sha256 (hash.data(), reinterpret_cast<const unsigned char*> (installed.license.text.data()),
                            installed.license.text.size());
        std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
        sodium_bin2hex (hex.data(), hex.size(), hash.data(), hash.size());
        installed.license.sha256 = hex.data();
        const auto serialized = providerPackToJson (installed);
        REQUIRE (providerPackFromJson (serialized, PackDescriptorUse::InstalledReceipt));
        CHECK_FALSE (providerPackFromJson (serialized));
        return;
    }
    SECTION ("Traversal path") { cached["instruments"][0]["path"] = "../Piano.sfz"; }
    SECTION ("Mutable Git branch") { cached["release_id"] = "main"; }
    SECTION ("Tampered license text") { cached["license"]["text"] = "MIT License"; }
    SECTION ("Different download pin") { cached["download_url"] = "https://github.com/sfzinstruments/Grand-Piano/archive/main.zip"; }
    SECTION ("Invented upstream checksum") { cached["expected_sha256"] = kArchiveHash; }
    SECTION ("Unsafe instrument identity") { cached["instruments"][0]["id"] = "../id"; }
    SECTION ("Invalid field type") { cached["version_pinned"] = 1; }
    CHECK_FALSE (providerPackFromJson (cached.dump()));
    CHECK_FALSE (providerPackFromJson (cached.dump(), PackDescriptorUse::InstalledReceipt));
}

TEST_CASE ("SFZ repository artwork uses declared images and immutable repository paths", "[sfz][provider][artwork]")
{
    CHECK_FALSE (isInstrumentArtworkPath ("https://raw.githubusercontent.com/vendor/pack/logo/cover.png"));
    CHECK_FALSE (isInstrumentArtworkPath ("https://raw.githubusercontent.com/vendor/pack/Library%20logo.png"));
    CHECK (isInstrumentArtworkPath ("https://raw.githubusercontent.com/vendor/pack/Images/cover.png"));
    CHECK (isInstrumentArtworkPath ("https://raw.githubusercontent.com/vendor/DuskGui/main/Images/DuskGui.png"));
    CHECK (isInstrumentArtworkPath ("https://raw.githubusercontent.com/vendor/button-accordion/main/Images/cover.png"));
    CHECK (isInstrumentArtworkPath ("Images/button-accordion.png"));
    CHECK (isInstrumentArtworkPath ("Images/Button%20accordion.png"));
    CHECK (isInstrumentArtworkPath ("Images/button-accordions.png"));
    CHECK_FALSE (isInstrumentArtworkPath ("Images/Buttons/cover.png"));
    CHECK_FALSE (isInstrumentArtworkPath ("Images/play-button.png"));
    CHECK_FALSE (isInstrumentArtworkPath ("button-accordion/Buttons/cover.png"));
    CHECK_FALSE (isInstrumentArtworkPath ("Images/button-accordion-play-button.png"));
    CHECK_FALSE (isInstrumentArtworkPath ("Images/GUI/cover.png"));
    CHECK_FALSE (isInstrumentArtworkPath ("Images/GUI-panel/cover.png"));
    CHECK_FALSE (isInstrumentArtworkPath ("Images/Skins/cover.png"));
    FakeTransport transport;
    auto summary = githubSummary();
    transport.replies = githubReplies();
    SECTION ("Repository cover uses the same pinned commit")
    {
        transport.replies[2].json["tree"].push_back ({ { "path", "Images/cover.png" }, { "type", "blob" }, { "mode", "100644" } });
        const auto result = resolveProviderPack (transport, summary);
        REQUIRE (result);
        REQUIRE (result.pack);
        CHECK (result.pack->presentation.artworkUrl == "https://raw.githubusercontent.com/sfzinstruments/Grand-Piano/" + kCommit + "/Images/cover.png");
    }
    SECTION ("Logos and arbitrary repository pictures are not instrument artwork")
    {
        for (const auto* path : { "logo.png", "banner.jpg", "diagram.png", "Images/preview-waveform.png", "logo/cover.png", "GUI/instrument.jpg" })
            transport.replies[2].json["tree"].push_back ({ { "path", path }, { "type", "blob" }, { "mode", "100644" } });
        const auto result = resolveProviderPack (transport, summary);
        REQUIRE (result);
        CHECK (result.pack->presentation.artworkUrl.empty());
    }
    SECTION ("Explicit repository artwork takes precedence over guessed cover files")
    {
        transport.replies[0].json["image_url"] = "https://raw.githubusercontent.com/sfzinstruments/Grand-Piano/" + kCommit + "/declared.jpg";
        transport.replies[2].json["tree"].push_back ({ { "path", "Images/cover.png" }, { "type", "blob" }, { "mode", "100644" } });
        const auto result = resolveProviderPack (transport, summary);
        REQUIRE (result);
        CHECK (result.pack->presentation.artworkUrl == transport.replies[0].json["image_url"].get<std::string>());
    }
    SECTION ("Author-prefixed catalog instruments use the image actually declared by their page")
    {
        summary.locator = summary.name = "author.Grand-Piano";
        transport.replies[0].json["name"] = summary.locator;
        transport.replies.push_back ({ {}, 200, {}, "- name: Pianos\n  page: pianos\n  instruments:\n  - name: Grand Piano\n    url: https://creator.example/piano\n    short_description: Multisampled acoustic piano\n" });
        transport.replies.push_back ({ {}, 200, {}, "<img src='/assets/img/pianos/actual_cover.jpg'>" });
        const auto result = resolveProviderPack (transport, summary);
        REQUIRE (result);
        REQUIRE (result.pack);
        CHECK (result.pack->presentation.artworkUrl == "https://raw.githubusercontent.com/sfzinstruments/sfzinstruments.github.io/master/assets/img/pianos/actual_cover.jpg");
        CHECK (result.pack->presentation.categoryHint.find ("Multisampled acoustic piano") != std::string::npos);
    }
    SECTION ("Search descriptions survive unavailable optional metadata during resolve")
    {
        summary.presentation.categoryHint = "Punk/indie drums and unusual percussion played with brushes.";
        transport.replies[0].json["description"] = nullptr;
        transport.replies.push_back ({ {}, 403 });
        const auto result = resolveProviderPack (transport, summary);
        REQUIRE (result);
        REQUIRE (result.pack);
        CHECK (result.pack->presentation.categoryHint == summary.presentation.categoryHint);
        CHECK (result.pack->license.text == kLicenseText);
    }
    SECTION ("Artifact artwork cannot direct a request outside its provider")
    {
        for (const auto* url : { "https://untrusted.example/piano.jpg", "https://musical-artifacts.com.evil.invalid/a.jpg",
                                "https://user@musical-artifacts.com/a.jpg", "https://musical-artifacts.com:444/a.jpg" })
        {
            CAPTURE (url);
            auto item = artifact();
            item["image_url"] = url;
            FakeTransport unsafe;
            unsafe.replies = { { Json::array ({ item }) } };
            const auto result = browseProvider (unsafe, "musical-artifacts");
            REQUIRE (result);
            REQUIRE (result.items.size() == 1);
            CHECK (result.items.front().presentation.artworkUrl.empty());
        }
    }
    SECTION ("Artifact tags and explicit HTTPS artwork survive browsing")
    {
        auto item = artifact();
        item["tags"] = { "piano", "acoustic" };
        item["image_url"] = "https://musical-artifacts.com/images/piano.jpg";
        transport.replies = { { Json::array ({ item }) } };
        const auto result = browseProvider (transport, "musical-artifacts");
        REQUIRE (result);
        REQUIRE (result.items.size() == 1);
        CHECK (result.items.front().presentation.categoryHint.find ("acoustic") != std::string::npos);
        CHECK (result.items.front().presentation.artworkUrl == "https://musical-artifacts.com/images/piano.jpg");
    }
}

TEST_CASE ("SFZ provider requests can pause before using the transport", "[sfz][provider]")
{
    FakeTransport transport;
    OperationControl control;
    SECTION ("Paused before any request")
    {
        control.paused = [] { return true; };
        CHECK (browseProvider (transport, "sfzinstruments", 1, {}, control).status == OperationStatus::Paused);
        CHECK (resolveProviderPack (transport, githubSummary(), control).status == OperationStatus::Paused);
        CHECK (transport.requests.empty());
    }
    SECTION ("Pause during optional demo lookup is preserved")
    {
        bool paused = false;
        control.paused = [&paused] { return paused; };
        transport.replies = githubReplies();
        transport.replies.push_back ({ {}, 200, {}, "catalog", [&paused] { paused = true; } });
        const auto result = resolveProviderPack (transport, githubSummary(), control);
        CHECK (result.status == OperationStatus::Paused);
        CHECK_FALSE (result.pack);
        CHECK (transport.requests.size() == 5);
    }
    SECTION ("Search catalog lookup preserves pause and cancellation")
    {
        bool interrupted = false;
        OperationStatus expected = OperationStatus::Paused;
        SECTION ("Paused while reading the optional catalog")
        { control.paused = [&interrupted] { return interrupted; }; }
        SECTION ("Cancelled while reading the optional catalog")
        { control.cancelled = [&interrupted] { return interrupted; }; expected = OperationStatus::Cancelled; }
        transport.replies = { { Json::array ({ repository() }) },
            { {}, 200, {}, "catalog", [&interrupted] { interrupted = true; } } };
        const auto result = browseProvider (transport, "sfzinstruments", 1, "piano", control);
        CHECK (result.status == expected);
        CHECK (result.items.empty());
        CHECK (transport.requests.size() == 2);
    }
    SECTION ("Cancel during optional demo lookup is preserved")
    {
        bool cancelled = false;
        control.cancelled = [&cancelled] { return cancelled; };
        transport.replies = githubReplies();
        transport.replies.push_back ({ {}, 200, {}, "catalog", [&cancelled] { cancelled = true; } });
        const auto result = resolveProviderPack (transport, githubSummary(), control);
        CHECK (result.status == OperationStatus::Cancelled);
        CHECK_FALSE (result.pack);
    }
}

#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzProvider.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <array>
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
}

TEST_CASE ("SFZ provider browse queries preserve bounded API requests", "[sfz][provider]")
{
    FakeTransport transport;
    SECTION ("GitHub query is a local summary filter")
    {
        auto synth = repository (43, "Synth");
        synth["description"] = "A synthesizer collection";
        transport.replies = { { Json::array ({ repository(), synth }) } };
        const auto result = browseProvider (transport, "sfzinstruments", 1, "piano");
        REQUIRE (result);
        REQUIRE (result.items.size() == 1);
        CHECK (result.items.front().packId == "42");
    }
    SECTION ("Musical Artifacts query is encoded")
    {
        transport.replies = { { Json::array ({ artifact() }), 200, { { "x-total-pages", "3" } } } };
        const auto result = browseProvider (transport, "musical-artifacts", 1, "piano & bells");
        REQUIRE (result);
        CHECK (result.nextPage == 2);
        CHECK (transport.requests.front().url.find ("q=piano%20%26%20bells") != std::string::npos);
    }
    SECTION ("Unbounded page is rejected before transport")
    {
        CHECK_FALSE (browseProvider (transport, "sfzinstruments", 201));
        CHECK (transport.requests.empty());
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

TEST_CASE ("SFZ Instruments does not accept SPDX hints without actual matching licenses", "[sfz][provider]")
{
    FakeTransport transport;
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
    CHECK_FALSE (resolveProviderPack (transport, githubSummary()));
}

TEST_CASE ("SFZ Instruments accepts formal Creative Commons license text", "[sfz][provider]")
{
    FakeTransport transport;
    SECTION ("Formal attribution title")
    { transport.replies = githubReplies ("Creative Commons Attribution 4.0 International Public License\nTerms follow.\n"); }
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
    REQUIRE (result.items.size() == 2);
    CHECK (result.items.front().presentation.favorites == favorites);
    CHECK_FALSE (result.items.front().presentation.stars);
    CHECK (result.items.back().licenseHint == "CC-BY-NC-ND-4.0");
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
    FakeTransport transport;
    auto item = artifact();
    SECTION ("CC3.0 is not silently treated as CC4.0")
    {
        item["license"] = "by-nc-sa-3";
        transport.replies = { { item }, { artifactLicenses ("by-nc-sa-3", "Creative Commons Attribution-NonCommercial-ShareAlike 3.0") } };
        const auto result = resolveProviderPack (transport, artifactSummary());
        REQUIRE (result);
        CHECK (result.pack->license.id == "CC-BY-NC-SA-3.0");
        CHECK (result.pack->license.constraints.find ("NonCommercial") != std::string::npos);
        CHECK (result.pack->license.constraints.find ("ShareAlike") != std::string::npos);
    }
    SECTION ("ND constraint is displayed")
    {
        item["license"] = "by-nd";
        transport.replies = { { item }, { artifactLicenses ("by-nd", "Creative Commons Attribution-NoDerivs 4.0") } };
        const auto result = resolveProviderPack (transport, artifactSummary());
        REQUIRE (result);
        CHECK (result.pack->license.constraints.find ("NoDerivatives") != std::string::npos);
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
    SECTION ("Traversal path") { cached["instruments"][0]["path"] = "../Piano.sfz"; }
    SECTION ("Mutable Git branch") { cached["release_id"] = "main"; }
    SECTION ("Tampered license text") { cached["license"]["text"] = "MIT License"; }
    SECTION ("Different download pin") { cached["download_url"] = "https://github.com/sfzinstruments/Grand-Piano/archive/main.zip"; }
    SECTION ("Invented upstream checksum") { cached["expected_sha256"] = kArchiveHash; }
    SECTION ("Unsafe instrument identity") { cached["instruments"][0]["id"] = "../id"; }
    SECTION ("Invalid field type") { cached["version_pinned"] = 1; }
    CHECK_FALSE (providerPackFromJson (cached.dump()));
}

TEST_CASE ("SFZ repository artwork uses declared images and immutable repository paths", "[sfz][provider][artwork]")
{
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
    SECTION ("Author-prefixed catalog instruments use the image actually declared by their page")
    {
        summary.locator = summary.name = "author.Grand-Piano";
        transport.replies[0].json["name"] = summary.locator;
        transport.replies.push_back ({ {}, 200, {}, "- name: Pianos\n  page: pianos\n  instruments:\n  - name: Grand Piano\n    url: https://creator.example/piano\n" });
        transport.replies.push_back ({ {}, 200, {}, "<img src='/assets/img/pianos/actual_cover.jpg'>" });
        const auto result = resolveProviderPack (transport, summary);
        REQUIRE (result);
        REQUIRE (result.pack);
        CHECK (result.pack->presentation.artworkUrl == "https://sfzinstruments.github.io/assets/img/pianos/actual_cover.jpg");
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

#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzTransfer.h"
#include "engine/sfz/SfzNetworkPolicy.h"
#include "foundation/Fs.h"
#include "foundation/Json.h"

#include <sodium.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
using namespace duskstudio::sfz;

std::string sha256 (const std::string& source)
{
    REQUIRE (sodium_init() >= 0);
    std::array<unsigned char, crypto_hash_sha256_BYTES> bytes {};
    crypto_hash_sha256 (bytes.data(), reinterpret_cast<const unsigned char*> (source.data()), source.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
    sodium_bin2hex (hex.data(), hex.size(), bytes.data(), bytes.size());
    return hex.data();
}

struct TemporaryDownload
{
    TemporaryDownload()
    {
        REQUIRE (sodium_init() >= 0);
        root = std::filesystem::canonical (std::filesystem::temp_directory_path())
            / ("dusk-sfz-transfer-" + std::to_string (randombytes_random())
               + "-" + std::to_string (randombytes_random()));
        REQUIRE (std::filesystem::create_directories (root));
    }
    ~TemporaryDownload() { std::error_code error; std::filesystem::remove_all (dusk::fs::nativeIOPath (root), error); }
    std::filesystem::path part() const { return root / "archive.part"; }
    std::filesystem::path root;
};

std::string readFile (const std::filesystem::path& path)
{
    std::ifstream stream (dusk::fs::nativeIOPath (path), std::ios::binary);
    return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char>() };
}

struct FakeReply
{
    int status = 200;
    HttpHeaders headers;
    std::string body;
    std::size_t chunkBytes = 4;
    OperationStatus endStatus = OperationStatus::Complete;
    bool failBeforeHeaders = false;
};

class FakeTransport final : public HttpTransport
{
public:
    OperationResult perform (const HttpRequest& request, const HttpHeadersCallback& onHeaders,
                             const HttpBodyCallback& onBody, const OperationControl& control) override
    {
        requests.push_back (request);
        if (requests.size() > replies.size()) return { OperationStatus::Failed, "No scripted response" };
        const auto& reply = replies[requests.size() - 1];
        const auto aborted = [&]
        {
            const auto state = control.status();
            return OperationResult { state == OperationStatus::Complete ? OperationStatus::Failed : state, "Aborted" };
        };
        if (control.status() != OperationStatus::Complete) return aborted();
        if (reply.failBeforeHeaders) return { OperationStatus::Failed, "Scripted connection failure" };
        if (! onHeaders ({ reply.status, request.url, reply.headers })) return aborted();
        for (std::size_t offset = 0; offset < reply.body.size(); offset += reply.chunkBytes)
        {
            if (control.status() != OperationStatus::Complete) return aborted();
            if (! onBody (std::string_view (reply.body).substr (offset, reply.chunkBytes))) return aborted();
        }
        if (control.status() != OperationStatus::Complete) return aborted();
        return { reply.endStatus, reply.endStatus == OperationStatus::Complete ? "" : "Scripted connection failure" };
    }
    std::vector<FakeReply> replies;
    std::vector<HttpRequest> requests;
};

DownloadRequest request (const TemporaryDownload& temp, const std::string& digest = {})
{
    return { "https://example.test/archive.zip", temp.part(), digest, 1024 };
}

void pauseAfterPrefix (FakeTransport& transport, const DownloadRequest& download)
{
    transport.replies.push_back ({ 200, { { "content-length", "8" }, { "etag", "\"v1\"" } }, "abcdefgh" });
    bool paused = false;
    OperationControl control;
    control.paused = [&] { return paused; };
    control.progress = [&] (const OperationProgress& progress) { paused = progress.completedBytes >= 4; };
    const auto result = downloadFile (transport, download, control);
    REQUIRE (result.status == OperationStatus::Paused);
    REQUIRE (result.bytes == 4);
    REQUIRE (readFile (download.partPath) == "abcd");
    auto sidecar = download.partPath;
    sidecar += ".resume.json";
    REQUIRE (std::filesystem::exists (dusk::fs::nativeIOPath (sidecar)));
}
} // namespace

TEST_CASE ("SFZ transfer streams and hashes a bounded archive", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    transport.replies.push_back ({ 200, { { "content-length", "8" } }, "abcdefgh" });
    const auto result = downloadFile (transport, request (temp));
    REQUIRE (result);
    CHECK (result.bytes == 8);
    CHECK (result.sha256 == sha256 ("abcdefgh"));
    CHECK (readFile (temp.part()) == "abcdefgh");
    CHECK (transport.requests.front().timeoutSeconds == 0);
    CHECK (transport.requests.front().lowSpeedSeconds == 30);
}

TEST_CASE ("SFZ download inspection preserves usable state without writing", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    const auto download = request (temp);
    pauseAfterPrefix (transport, download);
    auto sidecar = temp.part();
    sidecar += ".resume.json";
    const auto original = readFile (sidecar);
    const auto partTime = std::filesystem::last_write_time (temp.part());
    const auto metadataTime = std::filesystem::last_write_time (sidecar);
    REQUIRE (inspectDownloadPart (temp.part(), download.maxBytes) == DownloadPartState::Reusable);
    CHECK (readFile (temp.part()) == "abcd");
    CHECK (readFile (sidecar) == original);
    CHECK (std::filesystem::last_write_time (temp.part()) == partTime);
    CHECK (std::filesystem::last_write_time (sidecar) == metadataTime);
    CHECK (transport.requests.size() == 1);
    const auto parsed = dusk::json::Json::parse (original);
    SECTION ("Malformed and inconsistent metadata is invalid")
    {
        auto missingUrl = parsed; missingUrl.erase ("url");
        auto insecureUrl = parsed; insecureUrl["url"] = "http://example.test/archive.zip";
        auto badDigest = parsed; badDigest["digest"] = "incomplete";
        auto weak = parsed; weak["etag"] = "W/\"v1\""; weak["last_modified"] = "";
        auto badEffective = parsed; badEffective["effective_url"] = "file:///archive.zip";
        auto wrongSize = parsed; wrongSize["completed_bytes"] = 8; wrongSize["actual_sha256"] = sha256 ("abcd");
        auto badActual = parsed; badActual["completed_bytes"] = 4; badActual["actual_sha256"] = "broken";
        auto mismatch = parsed; mismatch["completed_bytes"] = 4; mismatch["actual_sha256"] = sha256 ("abcd");
        mismatch["digest"] = sha256 ("abcdefgh");
        for (const auto& metadata : { missingUrl, insecureUrl, badDigest, weak, badEffective, wrongSize, badActual, mismatch })
        {
            const auto text = metadata.dump();
            CAPTURE (text);
            std::ofstream (sidecar, std::ios::binary | std::ios::trunc) << text;
            CHECK (inspectDownloadPart (temp.part(), download.maxBytes) == DownloadPartState::Invalid);
            CHECK (readFile (sidecar) == text);
            CHECK (readFile (temp.part()) == "abcd");
        }
        for (const auto& text : { std::string ("{"), std::string (16385, 'x') })
        {
            std::ofstream (sidecar, std::ios::binary | std::ios::trunc) << text;
            CHECK (inspectDownloadPart (temp.part(), download.maxBytes) == DownloadPartState::Invalid);
            CHECK (readFile (sidecar) == text);
        }
    }
    SECTION ("Completed metadata needs no entity validator but content is still rehashed")
    {
        auto completed = parsed;
        completed["etag"] = completed["last_modified"] = "";
        completed["completed_bytes"] = 4;
        completed["actual_sha256"] = sha256 ("abcd");
        std::ofstream (sidecar, std::ios::binary | std::ios::trunc) << completed.dump();
        CHECK (inspectDownloadPart (temp.part(), download.maxBytes) == DownloadPartState::Reusable);
        std::ofstream (temp.part(), std::ios::binary | std::ios::trunc) << "wxyz";
        CHECK (inspectDownloadPart (temp.part(), download.maxBytes) == DownloadPartState::Reusable);
        CHECK_FALSE (downloadFile (transport, download));
        CHECK (transport.requests.size() == 1);
    }
    SECTION ("Missing paths and oversized or empty parts stay untouched")
    {
        CHECK (inspectDownloadPart (temp.part(), 3) == DownloadPartState::Invalid);
        REQUIRE (std::filesystem::remove (sidecar));
        CHECK (inspectDownloadPart (temp.part(), download.maxBytes) == DownloadPartState::Invalid);
        CHECK_FALSE (std::filesystem::exists (sidecar));
        std::ofstream (temp.part(), std::ios::binary | std::ios::trunc);
        CHECK (inspectDownloadPart (temp.part(), download.maxBytes) == DownloadPartState::Invalid);
        REQUIRE (std::filesystem::remove (temp.part()));
        CHECK (inspectDownloadPart (temp.part(), download.maxBytes) == DownloadPartState::Invalid);
        CHECK_FALSE (std::filesystem::exists (temp.part()));
        CHECK (inspectDownloadPart (temp.root / "absent" / "archive.part", download.maxBytes) == DownloadPartState::Invalid);
        CHECK_FALSE (std::filesystem::exists (temp.root / "absent"));
    }
    SECTION ("Ownership failure leaves an existing prefix unexamined")
    {
        const auto lockFile = temp.root / ".dusk-sfz.lock";
        REQUIRE (std::filesystem::remove (lockFile));
        REQUIRE (std::filesystem::create_directory (lockFile));
        const auto result = downloadFile (transport, download);
        CHECK (result.status == OperationStatus::Failed);
        CHECK (result.preservePart);
        CHECK (readFile (temp.part()) == "abcd");
        CHECK (readFile (sidecar) == original);
        CHECK (transport.requests.size() == 1);
    }
    SECTION ("Uninspectable metadata is preserved")
    {
        const auto linked = temp.root / "metadata-link";
        std::error_code error;
        std::filesystem::create_hard_link (sidecar, linked, error);
        if (error) SKIP ("This filesystem cannot create the metadata hard-link fixture");
        CHECK (inspectDownloadPart (temp.part(), download.maxBytes) == DownloadPartState::Unavailable);
        CHECK (readFile (sidecar) == original);
        CHECK (readFile (linked) == original);
        CHECK (readFile (temp.part()) == "abcd");
    }
}

TEST_CASE ("SFZ transfer verifies an upstream digest before completion", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    transport.replies.push_back ({ 200, { { "etag", "\"v1\"" } }, "abcdefgh" });
    SECTION ("Matching digest")
    {
        const auto result = downloadFile (transport, request (temp, sha256 ("abcdefgh")));
        REQUIRE (result);
        CHECK (result.sha256 == sha256 ("abcdefgh"));
    }
    SECTION ("Mismatched digest preserves part and exposes actual digest")
    {
        const auto result = downloadFile (transport, request (temp, sha256 ("different")));
        CHECK_FALSE (result);
        CHECK (result.error.find ("SHA-256") != std::string::npos);
        CHECK (result.sha256 == sha256 ("abcdefgh"));
        CHECK (readFile (temp.part()) == "abcdefgh");
        CHECK_FALSE (result.preservePart);
    }
}

TEST_CASE ("SFZ transfer rejects truncated or oversized archives", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    auto download = request (temp);
    SECTION ("Declared body is truncated")
    {
        transport.replies.push_back ({ 200, { { "content-length", "8" } }, "abcd" });
    }
    SECTION ("Declared size exceeds limit before writes")
    {
        download.maxBytes = 4;
        transport.replies.push_back ({ 200, { { "content-length", "8" } }, "abcdefgh" });
    }
    SECTION ("Undeclared body exceeds limit while streaming")
    {
        download.maxBytes = 4;
        transport.replies.push_back ({ 200, {}, "abcdefgh" });
    }
    SECTION ("Body exceeds its declared length")
    {
        transport.replies.push_back ({ 200, { { "content-length", "4" } }, "abcdefgh" });
    }
    SECTION ("Compressed HTTP body is rejected")
    {
        transport.replies.push_back ({ 200, { { "content-encoding", "gzip" } }, "abcdefgh" });
    }
    const auto result = downloadFile (transport, download);
    CHECK_FALSE (result);
    CHECK (std::filesystem::file_size (temp.part()) <= download.maxBytes);
}

TEST_CASE ("SFZ transfer resumes a paused archive using its entity validator", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    auto download = request (temp, sha256 ("abcdefgh"));
    SECTION ("Ordinary download path") {}
    SECTION ("Deep download path")
    {
        auto directory = temp.root;
        for (int index = 0; index < 4; ++index) directory /= std::string (72, char ('a' + index));
        download.partPath = directory / "archive.part";
    }
    pauseAfterPrefix (transport, download);
    REQUIRE (inspectDownloadPart (download.partPath, download.maxBytes) == DownloadPartState::Reusable);
    transport.replies.push_back ({ 206, { { "content-length", "4" }, { "content-range", "bytes 4-7/8" },
                                          { "etag", "\"v1\"" } }, "efgh" });
    const auto result = downloadFile (transport, download);
    REQUIRE (result);
    CHECK (transport.requests.back().headers.at ("range") == "bytes=4-");
    CHECK (transport.requests.back().headers.at ("if-range") == "\"v1\"");
    CHECK (readFile (download.partPath) == "abcdefgh");
    CHECK (result.sha256 == sha256 ("abcdefgh"));
    CHECK (inspectDownloadPart (download.partPath, download.maxBytes) == DownloadPartState::Reusable);
}

TEST_CASE ("SFZ transfer does not append a mismatched resume response", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    const auto download = request (temp);
    pauseAfterPrefix (transport, download);
    FakeReply reply { 206, { { "content-range", "bytes 4-7/8" }, { "etag", "\"v1\"" } }, "efgh" };
    SECTION ("Wrong offset") { reply.headers["content-range"] = "bytes 3-7/8"; }
    SECTION ("Changed entity") { reply.headers["etag"] = "\"v2\""; }
    SECTION ("Missing entity") { reply.headers.erase ("etag"); }
    SECTION ("Unsatisfiable range") { reply.status = 416; }
    SECTION ("Partial response stops before entity end") { reply.headers["content-range"] = "bytes 4-5/8"; }
    transport.replies.push_back (reply);
    const auto result = downloadFile (transport, download);
    CHECK_FALSE (result);
    CHECK_FALSE (result.preservePart);
    CHECK (readFile (temp.part()) == "abcd");
    CHECK (transport.requests.size() == 2);
}

TEST_CASE ("SFZ transfer retains validated prefixes across transient failures", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    const auto download = request (temp, sha256 ("abcdefgh"));
    FakeReply reply { 200, { { "content-length", "8" }, { "etag", "\"v1\"" } }, "abcd", 4, OperationStatus::Failed };
    bool existing = false;
    std::string prefix = "abcd";
    SECTION ("Connection drops after a validated prefix") {}
    SECTION ("A completed response contains only a prefix") { reply.endStatus = OperationStatus::Complete; }
    SECTION ("An existing prefix survives a connection failure before headers")
    {
        existing = true;
        reply.failBeforeHeaders = true;
    }
    SECTION ("An existing prefix survives a server error")
    {
        existing = true;
        reply = { 503, {}, "Unavailable" };
    }
    SECTION ("A chunked resume response ends before its declared range")
    {
        existing = true;
        reply = { 206, { { "content-range", "bytes 4-7/8" }, { "etag", "\"v1\"" } }, "ef" };
        prefix = "abcdef";
    }
    if (existing) pauseAfterPrefix (transport, download);
    transport.replies.push_back (reply);
    const auto interrupted = downloadFile (transport, download);
    REQUIRE_FALSE (interrupted);
    REQUIRE (interrupted.preservePart);
    REQUIRE (interrupted.bytes == prefix.size());
    REQUIRE (readFile (temp.part()) == prefix);
    const auto tail = std::string ("abcdefgh").substr (prefix.size());
    transport.replies.push_back ({ 206, { { "content-range", "bytes " + std::to_string (prefix.size()) + "-7/8" },
                                          { "etag", "\"v1\"" }, { "content-length", std::to_string (tail.size()) } }, tail });
    const auto result = downloadFile (transport, download);
    REQUIRE (result);
    CHECK (transport.requests.back().headers.at ("range") == "bytes=" + std::to_string (prefix.size()) + "-");
    CHECK (result.sha256 == sha256 ("abcdefgh"));
    CHECK (readFile (temp.part()) == "abcdefgh");
}

TEST_CASE ("SFZ transfer preserves validated prefixes while hashing is interrupted", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    const auto download = request (temp, sha256 ("abcdefgh"));
    pauseAfterPrefix (transport, download);
    bool cancelled = false, interrupted = false;
    SECTION ("Pause") {}
    SECTION ("Cancellation leaves deletion to the library service") { cancelled = true; }
    OperationControl control;
    control.paused = [&] { return interrupted && ! cancelled; };
    control.cancelled = [&] { return interrupted && cancelled; };
    control.progress = [&] (const OperationProgress&) { interrupted = true; };
    const auto result = downloadFile (transport, download, control);
    CHECK (result.status == (cancelled ? OperationStatus::Cancelled : OperationStatus::Paused));
    CHECK (result.preservePart);
    CHECK (result.bytes == 4);
    CHECK (transport.requests.size() == 1);
    transport.replies.push_back ({ 206, { { "content-range", "bytes 4-7/8" }, { "etag", "\"v1\"" } }, "efgh" });
    REQUIRE (downloadFile (transport, download));
    CHECK (readFile (temp.part()) == "abcdefgh");
}

TEST_CASE ("SFZ transfer safely restarts when the server ignores its range", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    const auto download = request (temp);
    pauseAfterPrefix (transport, download);
    transport.replies.push_back ({ 200, { { "content-length", "8" }, { "etag", "\"v2\"" } }, "12345678" });
    const auto result = downloadFile (transport, download);
    REQUIRE (result);
    CHECK (readFile (temp.part()) == "12345678");
    CHECK (result.sha256 == sha256 ("12345678"));
    CHECK (result.bytes == 8);
}

TEST_CASE ("SFZ transfer restarts unrelated parts and unvalidated entities", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    auto download = request (temp);
    SECTION ("URL changed")
    {
        pauseAfterPrefix (transport, download);
        download.url = "https://example.test/other.zip";
    }
    SECTION ("Expected digest changed")
    {
        pauseAfterPrefix (transport, download);
        download.expectedSha256 = sha256 ("12345678");
    }
    SECTION ("No entity validator")
    {
        transport.replies.push_back ({ 200, {}, "abcd", 4, OperationStatus::Failed });
        REQUIRE_FALSE (downloadFile (transport, download));
    }
    SECTION ("No sidecar")
    {
        std::ofstream stream (temp.part(), std::ios::binary);
        stream << "abcd";
    }
    transport.replies.push_back ({ 200, {}, "12345678" });
    REQUIRE (downloadFile (transport, download));
    CHECK (transport.requests.back().headers.count ("range") == 0);
    CHECK (readFile (temp.part()) == "12345678");
}

TEST_CASE ("SFZ transfer preserves cancelled parts and can later resume them", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    const auto download = request (temp);
    transport.replies.push_back ({ 200, { { "etag", "\"v1\"" } }, "abcdefgh" });
    bool cancelled = false;
    OperationControl control;
    control.cancelled = [&] { return cancelled; };
    control.progress = [&] (const OperationProgress&) { cancelled = true; };
    const auto result = downloadFile (transport, download, control);
    REQUIRE (result.status == OperationStatus::Cancelled);
    CHECK (readFile (temp.part()) == "abcd");
    transport.replies.push_back ({ 206, { { "etag", "\"v1\"" }, { "content-range", "bytes 4-7/8" } }, "efgh" });
    REQUIRE (downloadFile (transport, download));
    CHECK (readFile (temp.part()) == "abcdefgh");
}

TEST_CASE ("SFZ transfer can pause while hashing an existing prefix", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    const auto download = request (temp);
    pauseAfterPrefix (transport, download);
    bool paused = false;
    OperationControl control;
    control.paused = [&] { return paused; };
    control.progress = [&] (const OperationProgress&) { paused = true; };
    const auto result = downloadFile (transport, download, control);
    CHECK (result.status == OperationStatus::Paused);
    CHECK (transport.requests.size() == 1);
    CHECK (readFile (temp.part()) == "abcd");
}

TEST_CASE ("SFZ transfer checks control between bounded transport chunks", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    auto download = request (temp);
    download.maxBytes = 65536;
    transport.replies.push_back ({ 200, { { "etag", "\"v1\"" } }, std::string (65536, 'x'), 65536 });
    bool paused = false;
    OperationControl control;
    control.paused = [&] { return paused; };
    control.progress = [&] (const OperationProgress&) { paused = true; };
    const auto result = downloadFile (transport, download, control);
    CHECK (result.status == OperationStatus::Paused);
    CHECK (result.bytes == 32768);
    CHECK (std::filesystem::file_size (temp.part()) == 32768);
}

TEST_CASE ("SFZ transfer rehashes completed archives before reusing them", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    const auto download = request (temp, sha256 ("abcdefgh"));
    transport.replies.push_back ({ 200, {}, "abcdefgh" });
    REQUIRE (downloadFile (transport, download));
    SECTION ("Unchanged completed part needs no network")
    {
        const auto result = downloadFile (transport, download);
        REQUIRE (result);
        CHECK (result.bytes == 8);
        CHECK (result.sha256 == sha256 ("abcdefgh"));
    }
    SECTION ("Same-size tampering is rejected")
    {
        std::ofstream (temp.part(), std::ios::binary | std::ios::trunc) << "12345678";
        CHECK_FALSE (downloadFile (transport, download));
    }
    SECTION ("Changed length is rejected")
    {
        std::ofstream (temp.part(), std::ios::binary | std::ios::trunc) << "1234";
        CHECK_FALSE (downloadFile (transport, download));
    }
    SECTION ("Completed-part hashing remains pause-aware")
    {
        bool paused = false;
        OperationControl control;
        control.paused = [&] { return paused; };
        control.progress = [&] (const OperationProgress&) { paused = true; };
        CHECK (downloadFile (transport, download, control).status == OperationStatus::Paused);
    }
    CHECK (transport.requests.size() == 1);
}

TEST_CASE ("SFZ HTTP redirects remain bounded HTTPS requests", "[sfz][transfer]")
{
    FakeTransport transport;
    SECTION ("HTTPS and relative redirects succeed")
    {
        transport.replies = { { 302, { { "location", "/second" } }, "ignored" },
                              { 307, { { "location", "https://cdn.test/data" } }, "" },
                              { 200, { { "content-length", "2" } }, "{}" } };
        const auto result = fetchHttpText (transport, "https://example.test/first", 16);
        REQUIRE (result);
        CHECK (result.response.url == "https://cdn.test/data");
        CHECK (result.body == "{}");
        CHECK (transport.requests[1].url == "https://example.test/second");
    }
    SECTION ("Downgrade is rejected before a second request")
    {
        transport.replies = { { 302, { { "location", "http://example.test/data" } }, "" } };
        CHECK_FALSE (fetchHttpText (transport, "https://example.test/data", 16));
        CHECK (transport.requests.size() == 1);
    }
    SECTION ("Credentials in redirect are rejected")
    {
        transport.replies = { { 302, { { "location", "https://user:pass@example.test/data" } }, "" } };
        CHECK_FALSE (fetchHttpText (transport, "https://example.test/data", 16));
        CHECK (transport.requests.size() == 1);
    }
    SECTION ("Explicit hosts admit relative redirects and an allowed CDN")
    {
        transport.replies = { { 302, { { "location", "/second" } }, "" },
                              { 307, { { "location", "https://cdn.test/data" } }, "" },
                              { 200, {}, "{}" } };
        HttpRequest request;
        request.url = "https://EXAMPLE.TEST:443/first";
        request.allowedHosts = { "example.test", "cdn.test" };
        const auto result = fetchHttpText (transport, request, 16);
        REQUIRE (result);
        CHECK (result.body == "{}");
        CHECK (transport.requests.size() == 3);
    }
    SECTION ("Explicit hosts reject an unsafe initial destination without a request")
    {
        HttpRequest request;
        request.url = "https://elsewhere.test/data";
        request.allowedHosts = { "example.test" };
        CHECK_FALSE (fetchHttpText (transport, request, 16));
        CHECK (transport.requests.empty());
    }
    SECTION ("Explicit hosts reject a redirect before contacting the other destination")
    {
        for (const auto* destination : { "https://elsewhere.test/data", "https://example.test.evil.invalid/data",
                                        "https://example.test:444/data", "https://user@example.test/data" })
        {
            CAPTURE (destination);
            FakeTransport redirected;
            redirected.replies = { { 302, { { "location", destination } }, "" } };
            HttpRequest request;
            request.url = "https://example.test/data";
            request.allowedHosts = { "example.test" };
            CHECK_FALSE (fetchHttpText (redirected, request, 16));
            CHECK (redirected.requests.size() == 1);
        }
        CHECK_FALSE (httpsUrlOnHosts ("https://example.test/data", {}));
        auto embeddedNull = std::string ("https://example.test/data");
        embeddedNull += '\0';
        embeddedNull += "elsewhere.test";
        CHECK_FALSE (httpsUrlOnHosts (embeddedNull, { "example.test" }));
    }
    SECTION ("Redirect loops stop after five redirects")
    {
        transport.replies.assign (6, { 302, { { "location", "/loop" } }, "" });
        CHECK_FALSE (fetchHttpText (transport, "https://example.test/loop", 16));
        CHECK (transport.requests.size() == 6);
    }
}

TEST_CASE ("SFZ transfer drops range when a redirect changes the saved endpoint", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    const auto download = request (temp);
    pauseAfterPrefix (transport, download);
    transport.replies.push_back ({ 302, { { "location", "https://different.test/archive" } }, "" });
    transport.replies.push_back ({ 200, {}, "12345678" });
    REQUIRE (downloadFile (transport, download));
    CHECK (transport.requests.back().headers.count ("range") == 0);
    CHECK (transport.requests.back().headers.count ("if-range") == 0);
    CHECK (readFile (temp.part()) == "12345678");
}

TEST_CASE ("SFZ HTTP metadata is bounded and retains server status headers", "[sfz][transfer]")
{
    FakeTransport transport;
    SECTION ("Rate limit response is available to the caller")
    {
        transport.replies = { { 429, { { "Retry-After", "60" } }, "{}" } };
        const auto result = fetchHttpText (transport, "https://example.test/catalog", 16);
        REQUIRE (result);
        CHECK (result.response.statusCode == 429);
        CHECK (result.response.headers.at ("retry-after") == "60");
    }
    SECTION ("Unbounded JSON response is stopped")
    {
        transport.replies = { { 200, {}, "123456789" } };
        CHECK_FALSE (fetchHttpText (transport, "https://example.test/catalog", 4));
    }
    SECTION ("Truncated JSON response is rejected")
    {
        transport.replies = { { 200, { { "content-length", "8" } }, "{}" } };
        CHECK_FALSE (fetchHttpText (transport, "https://example.test/catalog", 16));
    }
    SECTION ("Non-HTTPS URL never reaches the transport")
    {
        CHECK_FALSE (fetchHttpText (transport, "http://example.test/catalog", 16));
        CHECK (transport.requests.empty());
    }
}

TEST_CASE ("SFZ transfer refuses unsafe existing parts", "[sfz][transfer]")
{
    TemporaryDownload temp;
    FakeTransport transport;
    auto download = request (temp);
    SECTION ("Oversized part")
    {
        std::ofstream stream (temp.part(), std::ios::binary);
        stream << "12345";
        stream.close();
        download.maxBytes = 4;
        CHECK_FALSE (downloadFile (transport, download));
        CHECK (readFile (temp.part()) == "12345");
    }
    SECTION ("Symlink part")
    {
        const auto target = temp.root / "target";
        std::ofstream (target, std::ios::binary) << "untouched";
        std::error_code error;
        std::filesystem::create_symlink (target, temp.part(), error);
        if (! error)
        {
            CHECK_FALSE (downloadFile (transport, download));
            CHECK (readFile (target) == "untouched");
        }
        else SUCCEED ("Host does not permit symlink creation");
    }
    CHECK (transport.requests.empty());
}

TEST_CASE ("SFZ connections exclude private and special-purpose IP destinations", "[sfz][transfer]")
{
    SECTION ("IPv4")
    {
        for (const std::array<std::uint8_t, 4> ip : {
            std::array<std::uint8_t, 4> { 8, 8, 8, 8 },
            { 1, 1, 1, 1 }, { 100, 63, 255, 255 }, { 100, 128, 0, 1 },
            { 172, 15, 255, 255 }, { 172, 32, 0, 1 }, { 223, 255, 255, 254 } })
        {
            CAPTURE (ip);
            CHECK (publicIpv4 (ip));
        }
        for (const std::array<std::uint8_t, 4> ip : {
            std::array<std::uint8_t, 4> { 0, 0, 0, 0 },
            { 0, 12, 34, 56 }, { 10, 255, 255, 255 }, { 127, 0, 0, 1 },
            { 100, 64, 0, 0 }, { 100, 127, 255, 255 }, { 169, 254, 1, 1 },
            { 172, 16, 0, 1 }, { 172, 31, 255, 255 }, { 192, 0, 0, 170 },
            { 192, 0, 2, 1 }, { 192, 88, 99, 1 }, { 192, 168, 1, 1 },
            { 198, 18, 0, 1 }, { 198, 19, 255, 255 }, { 198, 51, 100, 1 },
            { 203, 0, 113, 1 }, { 224, 0, 0, 1 }, { 240, 0, 0, 1 }, { 255, 255, 255, 255 } })
        {
            CAPTURE (ip);
            CHECK_FALSE (publicIpv4 (ip));
        }
    }
    SECTION ("IPv6 and embedded IPv4")
    {
        const auto bytes = [] (std::array<std::uint16_t, 8> words)
        {
            std::array<std::uint8_t, 16> result {};
            for (std::size_t i = 0; i < words.size(); ++i)
            {
                result[2 * i] = static_cast<std::uint8_t> (words[i] >> 8);
                result[2 * i + 1] = static_cast<std::uint8_t> (words[i]);
            }
            return result;
        };
        for (const auto words : {
            std::array<std::uint16_t, 8> { 0x2001, 0x4860, 0x4860, 0, 0, 0, 0, 0x8888 },
            { 0x2606, 0x4700, 0x4700, 0, 0, 0, 0, 0x1111 },
            { 0, 0, 0, 0, 0, 0xffff, 0x0808, 0x0808 },
            { 0x64, 0xff9b, 0, 0, 0, 0, 0x0808, 0x0808 } })
        {
            CAPTURE (words);
            CHECK (publicIpv6 (bytes (words)));
        }
        for (const auto words : {
            std::array<std::uint16_t, 8> {}, { 0, 0, 0, 0, 0, 0, 0, 1 },
            { 0xfc00, 0, 0, 0, 0, 0, 0, 1 }, { 0xfe80, 0, 0, 0, 0, 0, 0, 1 },
            { 0xff02, 0, 0, 0, 0, 0, 0, 1 }, { 0x2001, 0x0db8, 0, 0, 0, 0, 0, 1 },
            { 0x2001, 0, 0, 0, 0, 0, 0, 1 }, { 0x2002, 0x7f00, 1, 0, 0, 0, 0, 1 },
            { 0x3fff, 0x0fff, 0, 0, 0, 0, 0, 1 },
            { 0, 0, 0, 0, 0, 0xffff, 0x7f00, 1 },
            { 0, 0, 0, 0, 0, 0xffff, 0x0a00, 1 },
            { 0x64, 0xff9b, 0, 0, 0, 0, 0xc0a8, 1 } })
        {
            CAPTURE (words);
            CHECK_FALSE (publicIpv6 (bytes (words)));
        }
    }
}

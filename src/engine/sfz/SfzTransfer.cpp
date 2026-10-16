#if defined(_WIN32) && ! defined(NOMINMAX)
 #define NOMINMAX
#endif

#include "SfzTransfer.h"
#include "../../foundation/Json.h"

#include <curl/curl.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <utility>

#if defined(_WIN32)
 #include <windows.h>
 #include <fcntl.h>
 #include <io.h>
#else
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace duskstudio::sfz
{
namespace
{
constexpr std::size_t kChunkBytes = 32768;
constexpr std::size_t kHeaderBytes = 65536;

bool curlReady()
{
    static const auto initialization = curl_global_init (CURL_GLOBAL_DEFAULT);
    return initialization == CURLE_OK;
}

std::string lower (std::string value)
{
    for (auto& c : value)
        if (c >= 'A' && c <= 'Z') c = static_cast<char> (c + ('a' - 'A'));
    return value;
}

std::string trim (std::string_view value)
{
    while (! value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix (1);
    while (! value.empty() && (value.back() == ' ' || value.back() == '\t'
                               || value.back() == '\r' || value.back() == '\n')) value.remove_suffix (1);
    return std::string (value);
}

std::string header (const HttpResponse& response, const char* key)
{
    const auto found = response.headers.find (key);
    return found == response.headers.end() ? std::string{} : found->second;
}

std::optional<std::uint64_t> number (std::string_view value)
{
    std::uint64_t result = 0;
    const auto parsed = std::from_chars (value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return {};
    return result;
}

bool validDigest (std::string_view digest)
{
    return digest.size() == 64 && std::all_of (digest.begin(), digest.end(), [] (char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

std::optional<std::string> httpsUrl (const std::string& source, const std::string& base = {})
{
    if (! curlReady() || source.empty() || source.size() > 8192
        || source.find_first_of ("\r\n\t") != std::string::npos) return {};
    using Url = std::unique_ptr<CURLU, decltype (&curl_url_cleanup)>;
    Url url (curl_url(), &curl_url_cleanup);
    if (! url || (! base.empty() && curl_url_set (url.get(), CURLUPART_URL, base.c_str(), 0) != CURLUE_OK)
        || curl_url_set (url.get(), CURLUPART_URL, source.c_str(), 0) != CURLUE_OK) return {};
    const auto get = [&] (CURLUPart part)
    {
        char* value = nullptr;
        const auto code = curl_url_get (url.get(), part, &value, 0);
        std::string result = code == CURLUE_OK ? value : "";
        curl_free (value);
        return result;
    };
    const auto has = [&] (CURLUPart part)
    {
        char* value = nullptr;
        const auto present = curl_url_get (url.get(), part, &value, 0) == CURLUE_OK;
        curl_free (value);
        return present;
    };
    if (lower (get (CURLUPART_SCHEME)) != "https" || get (CURLUPART_HOST).empty()
        || has (CURLUPART_USER) || has (CURLUPART_PASSWORD) || has (CURLUPART_OPTIONS)) return {};
    curl_url_set (url.get(), CURLUPART_FRAGMENT, nullptr, 0);
    return get (CURLUPART_URL);
}

bool safeRequest (const HttpRequest& request)
{
    if (! httpsUrl (request.url) || request.connectTimeoutSeconds <= 0 || request.timeoutSeconds <= 0
        || request.lowSpeedBytesPerSecond <= 0 || request.lowSpeedSeconds <= 0) return false;
    for (const auto& item : request.headers)
    {
        const auto name = lower (item.first);
        if (name.empty() || name.find_first_of ("\r\n: \t") != std::string::npos
            || item.second.find_first_of ("\r\n") != std::string::npos
            || name == "authorization" || name == "proxy-authorization" || name == "cookie") return false;
    }
    return true;
}

OperationResult requestHttps (HttpTransport& transport, HttpRequest request,
                              const HttpHeadersCallback& onHeaders, const HttpBodyCallback& onBody,
                              const OperationControl& control, const std::string& resumeUrl = {})
{
    const auto initialUrl = httpsUrl (request.url);
    if (! initialUrl || ! safeRequest (request)) return { OperationStatus::Failed, "Invalid HTTPS request" };
    request.url = *initialUrl;
    const auto started = std::chrono::steady_clock::now();
    const auto timeout = request.timeoutSeconds;
    for (int redirects = 0; redirects <= 5; ++redirects)
    {
        const auto state = control.status();
        if (state != OperationStatus::Complete) return { state, {} };
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds> (
            std::chrono::steady_clock::now() - started).count();
        if (elapsed >= timeout) return { OperationStatus::Failed, "HTTPS request timed out" };
        request.timeoutSeconds = timeout - static_cast<long> (elapsed);
        bool redirect = false;
        bool receivedHeaders = false;
        std::size_t redirectBytes = 0;
        std::string location;
        std::string error;
        auto result = transport.perform (request, [&] (const HttpResponse& source)
        {
            if (receivedHeaders) { error = "Multiple HTTP response bodies"; return false; }
            receivedHeaders = true;
            auto response = source;
            response.url = request.url;
            HttpHeaders normalized;
            for (const auto& item : response.headers)
                if (! normalized.emplace (lower (item.first), item.second).second)
                { error = "Duplicate HTTP response header"; return false; }
            response.headers = std::move (normalized);
            redirect = response.statusCode == 301 || response.statusCode == 302
                || response.statusCode == 303 || response.statusCode == 307 || response.statusCode == 308;
            if (redirect) location = header (response, "location");
            return redirect || onHeaders (response);
        }, [&] (std::string_view chunk)
        {
            if (! receivedHeaders) { error = "HTTP body arrived before headers"; return false; }
            if (redirect)
            {
                if (chunk.size() > kHeaderBytes - redirectBytes)
                { error = "HTTPS redirect body exceeds the size limit"; return false; }
                redirectBytes += chunk.size();
                return true;
            }
            for (std::size_t offset = 0; offset < chunk.size(); offset += kChunkBytes)
            {
                if (control.status() != OperationStatus::Complete) return false;
                if (! onBody (chunk.substr (offset, std::min (kChunkBytes, chunk.size() - offset)))) return false;
            }
            return true;
        }, control);
        if (! error.empty()) return { OperationStatus::Failed, error };
        const auto currentState = control.status();
        if (currentState != OperationStatus::Complete) return { currentState, {} };
        if (! result) return result;
        if (! receivedHeaders) return { OperationStatus::Failed, "HTTP response has no headers" };
        if (! redirect) return result;
        if (redirects == 5) return { OperationStatus::Failed, "HTTPS redirect limit exceeded" };
        const auto destination = httpsUrl (location, request.url);
        if (! destination) return { OperationStatus::Failed, "Redirect is not an HTTPS URL" };
        if (*destination != resumeUrl)
        {
            request.headers.erase ("range");
            request.headers.erase ("if-range");
        }
        request.url = *destination;
    }
    return { OperationStatus::Failed, "HTTPS redirect limit exceeded" };
}

struct CurlCallbacks
{
    const HttpHeadersCallback& onHeaders;
    const HttpBodyCallback& onBody;
    const OperationControl& control;
    HttpResponse response;
    std::size_t headerBytes = 0;
    bool headersDelivered = false;
    std::string error;
};

std::size_t curlHeader (char* bytes, std::size_t size, std::size_t count, void* context) noexcept
{
    auto& state = *static_cast<CurlCallbacks*> (context);
    try
    {
        if (size != 1 || count > kHeaderBytes - state.headerBytes) return 0;
        state.headerBytes += count;
        const std::string_view line (bytes, count);
        if (line.substr (0, 5) == "HTTP/")
        {
            state.response.headers.clear();
            state.headersDelivered = false;
            const auto separator = line.find (' ');
            const auto status = separator == std::string_view::npos ? std::optional<std::uint64_t>{}
                : number (line.substr (separator + 1, 3));
            if (! status || *status < 100 || *status > 599) return 0;
            state.response.statusCode = static_cast<int> (*status);
        }
        else if (line == "\r\n" || line == "\n")
        {
            if (state.response.statusCode >= 200 && ! state.headersDelivered)
            {
                state.headersDelivered = true;
                if (! state.onHeaders (state.response)) return 0;
            }
        }
        else if (! state.headersDelivered)
        {
            const auto separator = line.find (':');
            if (separator == std::string_view::npos || state.response.headers.size() >= 128) return 0;
            const auto name = lower (trim (line.substr (0, separator)));
            const auto value = trim (line.substr (separator + 1));
            const auto inserted = state.response.headers.emplace (name, value);
            if (! inserted.second)
            {
                if (name == "content-length" || name == "content-range" || name == "etag"
                    || name == "last-modified" || name == "location" || name == "content-encoding") return 0;
                inserted.first->second += ", " + value;
            }
        }
        return count;
    }
    catch (...) { state.error = "HTTP header callback failed"; return 0; }
}

std::size_t curlBody (char* bytes, std::size_t size, std::size_t count, void* context) noexcept
{
    auto& state = *static_cast<CurlCallbacks*> (context);
    try
    {
        if (size != 1 || ! state.headersDelivered) return 0;
        for (std::size_t offset = 0; offset < count; offset += kChunkBytes)
        {
            if (state.control.status() != OperationStatus::Complete
                || ! state.onBody ({ bytes + offset, std::min (kChunkBytes, count - offset) })) return 0;
        }
        return count;
    }
    catch (...) { state.error = "HTTP body callback failed"; return 0; }
}

int curlProgress (void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept
{
    try { return static_cast<CurlCallbacks*> (context)->control.status() == OperationStatus::Complete ? 0 : 1; }
    catch (...) { return 1; }
}

struct CloseFile
{
    void operator() (std::FILE* handle) const noexcept { std::fclose (handle); }
};
using File = std::unique_ptr<std::FILE, CloseFile>;

bool safeFilePath (const std::filesystem::path& path)
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute (path, error);
    if (error || absolute.filename().empty()) return false;
    std::filesystem::path current;
    for (const auto& part : absolute)
    {
        current /= part;
        const auto status = std::filesystem::symlink_status (current, error);
        if (error && error != std::errc::no_such_file_or_directory) return false;
        error.clear();
        if (std::filesystem::is_symlink (status)) return false;
    }
    return true;
}

File openFile (const std::filesystem::path& path)
{
    if (! safeFilePath (path)) return File {};
   #if defined(_WIN32)
    const auto handle = CreateFileW (path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                                    nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return File {};
    BY_HANDLE_FILE_INFORMATION info {};
    if (! GetFileInformationByHandle (handle, &info) || info.nNumberOfLinks != 1
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
    { CloseHandle (handle); return File {}; }
    const auto fd = _open_osfhandle (reinterpret_cast<intptr_t> (handle), _O_BINARY | _O_RDWR);
    if (fd == -1) { CloseHandle (handle); return File {}; }
    auto* file = _fdopen (fd, "r+b");
    if (file == nullptr) _close (fd);
   #else
    const auto fd = ::open (path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd == -1) return File {};
    struct stat info {};
    if (::fstat (fd, &info) != 0 || ! S_ISREG (info.st_mode) || info.st_nlink != 1)
    { ::close (fd); return File {}; }
    auto* file = ::fdopen (fd, "r+b");
    if (file == nullptr) ::close (fd);
   #endif
    return File (file);
}

bool rewindFile (std::FILE* file, bool truncate)
{
    if (std::fflush (file) != 0) return false;
   #if defined(_WIN32)
    if (truncate && _chsize_s (_fileno (file), 0) != 0) return false;
    return _fseeki64 (file, 0, SEEK_SET) == 0;
   #else
    if (truncate && ::ftruncate (::fileno (file), 0) != 0) return false;
    return ::fseeko (file, 0, SEEK_SET) == 0;
   #endif
}

struct ResumeMetadata
{
    std::string url, digest, effectiveUrl, etag, lastModified;
    std::optional<std::uint64_t> completedBytes;
    std::string actualDigest;
    std::string validator() const
    {
        if (! etag.empty() && etag.substr (0, 2) != "W/") return etag;
        return lastModified;
    }
};

std::filesystem::path metadataPath (const std::filesystem::path& part)
{
    auto result = part;
    result += ".resume.json";
    return result;
}

ResumeMetadata readMetadata (const std::filesystem::path& path)
{
    if (! safeFilePath (path)) return {};
    std::error_code error;
    if (! std::filesystem::is_regular_file (path, error)
        || std::filesystem::file_size (path, error) > 16384 || error) return {};
    std::ifstream stream (path, std::ios::binary);
    std::array<char, 16385> bytes {};
    stream.read (bytes.data(), static_cast<std::streamsize> (bytes.size()));
    if (stream.gcount() == static_cast<std::streamsize> (bytes.size()) || stream.bad()) return {};
    const std::string content (bytes.data(), static_cast<std::size_t> (stream.gcount()));
    const auto json = dusk::json::Json::parse (content, nullptr, false);
    if (! json.is_object()) return {};
    const auto get = [&] (const char* key)
    {
        const auto found = json.find (key);
        return found != json.end() && found->is_string() ? found->get<std::string>() : std::string{};
    };
    std::optional<std::uint64_t> completed;
    const auto size = json.find ("completed_bytes");
    if (size != json.end() && size->is_number_unsigned()) completed = size->get<std::uint64_t>();
    return { get ("url"), get ("digest"), get ("effective_url"), get ("etag"), get ("last_modified"),
             completed, get ("actual_sha256") };
}

bool writeMetadata (const std::filesystem::path& path, const ResumeMetadata& metadata)
{
    auto json = dusk::json::Json {
        { "url", metadata.url }, { "digest", metadata.digest }, { "effective_url", metadata.effectiveUrl },
        { "etag", metadata.etag }, { "last_modified", metadata.lastModified }
    };
    if (metadata.completedBytes)
    {
        json["completed_bytes"] = *metadata.completedBytes;
        json["actual_sha256"] = metadata.actualDigest;
    }
    const auto content = json.dump();
    if (content.size() > 16384) return false;
    auto file = openFile (path);
    if (! file || ! rewindFile (file.get(), true)) return false;
    return std::fwrite (content.data(), 1, content.size(), file.get()) == content.size()
        && std::fflush (file.get()) == 0;
}

struct ByteRange { std::uint64_t first, last, total; };

std::optional<ByteRange> contentRange (std::string_view value)
{
    if (value.substr (0, 6) != "bytes ") return {};
    value.remove_prefix (6);
    const auto dash = value.find ('-'), slash = value.find ('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || dash >= slash) return {};
    const auto first = number (value.substr (0, dash));
    const auto last = number (value.substr (dash + 1, slash - dash - 1));
    const auto total = number (value.substr (slash + 1));
    if (! first || ! last || ! total || *first > *last || *last >= *total) return {};
    return ByteRange { *first, *last, *total };
}

std::string hashDigest (crypto_hash_sha256_state hash)
{
    std::array<unsigned char, crypto_hash_sha256_BYTES> bytes {};
    crypto_hash_sha256_final (&hash, bytes.data());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
    sodium_bin2hex (hex.data(), hex.size(), bytes.data(), bytes.size());
    return hex.data();
}
} // namespace

bool initializeHttpTransport()
{
    return curlReady();
}

OperationResult CurlTransport::perform (const HttpRequest& request, const HttpHeadersCallback& onHeaders,
                                         const HttpBodyCallback& onBody, const OperationControl& control)
{
    if (! safeRequest (request)) return { OperationStatus::Failed, "Invalid HTTPS request" };
    if (! curlReady()) return { OperationStatus::Failed, "Cannot initialize HTTPS transport" };
    using Handle = std::unique_ptr<CURL, decltype (&curl_easy_cleanup)>;
    Handle handle (curl_easy_init(), &curl_easy_cleanup);
    if (! handle) return { OperationStatus::Failed, "Cannot initialize HTTPS request" };
    CurlCallbacks callbacks { onHeaders, onBody, control, {}, 0, false, {} };
    std::array<char, CURL_ERROR_SIZE> errorBuffer {};
    CURLcode setting = CURLE_OK;
    const auto set = [&] (auto option, auto value)
    {
        const auto result = curl_easy_setopt (handle.get(), option, value);
        if (setting == CURLE_OK) setting = result;
    };
    set (CURLOPT_URL, request.url.c_str());
    set (CURLOPT_USERAGENT, "DuskStudio-SFZ/1");
    set (CURLOPT_FOLLOWLOCATION, 0L);
   #if LIBCURL_VERSION_NUM >= 0x075500
    const auto protocols = curl_easy_setopt (handle.get(), CURLOPT_PROTOCOLS_STR, "https");
    if (protocols == CURLE_UNKNOWN_OPTION)
    {
        CURL_IGNORE_DEPRECATION (set (CURLOPT_PROTOCOLS, static_cast<long> (CURLPROTO_HTTPS));)
    }
    else if (setting == CURLE_OK) setting = protocols;
    const auto redirectProtocols = curl_easy_setopt (handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    if (redirectProtocols == CURLE_UNKNOWN_OPTION)
    {
        CURL_IGNORE_DEPRECATION (set (CURLOPT_REDIR_PROTOCOLS, static_cast<long> (CURLPROTO_HTTPS));)
    }
    else if (setting == CURLE_OK) setting = redirectProtocols;
   #else
    set (CURLOPT_PROTOCOLS, static_cast<long> (CURLPROTO_HTTPS));
    set (CURLOPT_REDIR_PROTOCOLS, static_cast<long> (CURLPROTO_HTTPS));
   #endif
    set (CURLOPT_SSL_VERIFYPEER, 1L);
    set (CURLOPT_SSL_VERIFYHOST, 2L);
    set (CURLOPT_NETRC, static_cast<long> (CURL_NETRC_IGNORED));
    set (CURLOPT_HTTPAUTH, static_cast<long> (CURLAUTH_NONE));
    set (CURLOPT_NOSIGNAL, 1L);
    set (CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    set (CURLOPT_HTTP_CONTENT_DECODING, 0L);
    set (CURLOPT_CONNECTTIMEOUT, request.connectTimeoutSeconds);
    set (CURLOPT_TIMEOUT, request.timeoutSeconds);
    set (CURLOPT_LOW_SPEED_LIMIT, request.lowSpeedBytesPerSecond);
    set (CURLOPT_LOW_SPEED_TIME, request.lowSpeedSeconds);
    set (CURLOPT_ERRORBUFFER, errorBuffer.data());
    set (CURLOPT_HEADERFUNCTION, &curlHeader);
    set (CURLOPT_HEADERDATA, &callbacks);
    set (CURLOPT_WRITEFUNCTION, &curlBody);
    set (CURLOPT_WRITEDATA, &callbacks);
    set (CURLOPT_XFERINFOFUNCTION, &curlProgress);
    set (CURLOPT_XFERINFODATA, &callbacks);
    set (CURLOPT_NOPROGRESS, 0L);
    using Headers = std::unique_ptr<curl_slist, decltype (&curl_slist_free_all)>;
    Headers headers (nullptr, &curl_slist_free_all);
    for (const auto& item : request.headers)
    {
        auto* appended = curl_slist_append (headers.get(), (item.first + ": " + item.second).c_str());
        if (! appended) return { OperationStatus::Failed, "Cannot allocate HTTP headers" };
        headers.release();
        headers.reset (appended);
    }
    set (CURLOPT_HTTPHEADER, headers.get());
    if (setting != CURLE_OK) return { OperationStatus::Failed, curl_easy_strerror (setting) };
    const auto result = curl_easy_perform (handle.get());
    const auto state = control.status();
    if (state != OperationStatus::Complete) return { state, {} };
    if (! callbacks.error.empty()) return { OperationStatus::Failed, callbacks.error };
    if (result != CURLE_OK)
        return { OperationStatus::Failed, errorBuffer[0] != '\0' ? errorBuffer.data() : curl_easy_strerror (result) };
    return { OperationStatus::Complete, {} };
}

HttpTextResult fetchHttpText (HttpTransport& transport, const HttpRequest& source,
                              std::uint64_t maxBytes, const OperationControl& control)
{
    HttpTextResult result;
    auto request = source;
    request.headers["accept-encoding"] = "identity";
    std::optional<std::uint64_t> length;
    std::string failure;
    const auto operation = requestHttps (transport, request, [&] (const HttpResponse& response)
    {
        result.response = response;
        const auto declaredLength = response.headers.find ("content-length");
        if (declaredLength != response.headers.end()) length = number (declaredLength->second);
        if ((declaredLength != response.headers.end() && ! length) || (length && *length > maxBytes))
        { failure = "HTTP body exceeds the size limit"; return false; }
        const auto encoding = lower (header (response, "content-encoding"));
        if (! encoding.empty() && encoding != "identity")
        { failure = "Unsupported HTTP content encoding"; return false; }
        return true;
    }, [&] (std::string_view chunk)
    {
        if (chunk.size() > maxBytes - result.body.size())
        { failure = "HTTP body exceeds the size limit"; return false; }
        result.body.append (chunk);
        return true;
    }, control);
    result.status = operation.status;
    result.error = failure.empty() ? operation.error : failure;
    if (! failure.empty()) result.status = OperationStatus::Failed;
    if (result && length && result.body.size() != *length)
    { result.status = OperationStatus::Failed; result.error = "Truncated HTTP body"; }
    return result;
}

HttpTextResult fetchHttpText (HttpTransport& transport, const std::string& url,
                              std::uint64_t maxBytes, const OperationControl& control)
{
    HttpRequest request;
    request.url = url;
    return fetchHttpText (transport, request, maxBytes, control);
}

DownloadResult downloadFile (HttpTransport& transport, const DownloadRequest& request,
                              const OperationControl& control)
{
    DownloadResult result;
    const auto fail = [&] (const std::string& message)
    { result.status = OperationStatus::Failed; result.error = message; return result; };
    const auto initialState = control.status();
    if (initialState != OperationStatus::Complete) { result.status = initialState; return result; }
    const auto url = httpsUrl (request.url);
    const auto digest = lower (request.expectedSha256);
    if (! url || request.maxBytes == 0 || (! digest.empty() && ! validDigest (digest)))
        return fail ("Invalid download request");
    if (sodium_init() < 0) return fail ("Cannot initialize SHA-256");
    const auto sidecar = metadataPath (request.partPath);
    if (! safeFilePath (request.partPath) || ! safeFilePath (sidecar)) return fail ("Unsafe download path");
    std::error_code error;
    const auto parent = request.partPath.parent_path();
    if (! parent.empty()) std::filesystem::create_directories (parent, error);
    if (error) return fail ("Cannot create download directory");
    auto file = openFile (request.partPath);
    if (! file) return fail ("Cannot open download part safely");
    auto offset = std::filesystem::file_size (request.partPath, error);
    if (error || offset > request.maxBytes) return fail ("Existing download part exceeds the size limit");
    auto metadata = readMetadata (sidecar);
    const bool sameRequest = metadata.url == *url && metadata.digest == digest;
    const bool completed = sameRequest && metadata.completedBytes && validDigest (metadata.actualDigest);
    if (completed && *metadata.completedBytes != offset) return fail ("Completed download part has changed size; restart the download");
    const bool resume = offset != 0 && sameRequest
        && ! metadata.validator().empty() && httpsUrl (metadata.effectiveUrl).has_value();
    if (! resume && ! completed)
    {
        if (! rewindFile (file.get(), true)) return fail ("Cannot restart download part");
        offset = 0;
    }
    crypto_hash_sha256_state hash {};
    crypto_hash_sha256_init (&hash);
    std::array<unsigned char, kChunkBytes> buffer {};
    std::uint64_t hashed = 0;
    while (hashed < offset)
    {
        const auto state = control.status();
        if (state != OperationStatus::Complete) { result.status = state; result.bytes = offset; return result; }
        const auto count = static_cast<std::size_t> (std::min<std::uint64_t> (buffer.size(), offset - hashed));
        if (std::fread (buffer.data(), 1, count, file.get()) != count) return fail ("Cannot hash existing download part");
        crypto_hash_sha256_update (&hash, buffer.data(), count);
        hashed += count;
        if (control.progress) control.progress ({ hashed, offset, 0 });
    }
    const auto afterHash = control.status();
    if (afterHash != OperationStatus::Complete) { result.status = afterHash; result.bytes = offset; return result; }
    if (completed)
    {
        result.bytes = offset;
        result.sha256 = hashDigest (hash);
        if (result.sha256 != metadata.actualDigest || (! digest.empty() && result.sha256 != digest))
            return fail ("Completed download part has changed; restart the download");
        result.status = OperationStatus::Complete;
        return result;
    }
    if (std::fseek (file.get(), 0, SEEK_CUR) != 0) return fail ("Cannot append download part");
    HttpRequest http;
    http.url = *url;
    http.timeoutSeconds = 1800;
    http.headers["accept-encoding"] = "identity";
    if (resume)
    {
        http.headers["range"] = "bytes=" + std::to_string (offset) + "-";
        http.headers["if-range"] = metadata.validator();
    }
    result.bytes = offset;
    std::optional<std::uint64_t> expectedBody, expectedTotal;
    std::uint64_t received = 0;
    bool accepted = false;
    std::string failure;
    const auto operation = requestHttps (transport, http, [&] (const HttpResponse& response)
    {
        const auto reject = [&] (const char* reason) { failure = reason; return false; };
        if (response.statusCode != 200 && response.statusCode != 206) return reject ("Download server rejected the request");
        const auto encoding = lower (header (response, "content-encoding"));
        if (! encoding.empty() && encoding != "identity") return reject ("Unsupported download content encoding");
        const auto declaredLength = response.headers.find ("content-length");
        if (declaredLength != response.headers.end())
        {
            expectedBody = number (declaredLength->second);
            if (! expectedBody) return reject ("Invalid download content length");
        }
        if (response.statusCode == 206)
        {
            const auto range = contentRange (header (response, "content-range"));
            if (! resume || ! range || range->first != offset || range->last != range->total - 1)
                return reject ("Download resume range does not match the existing part");
            const auto currentValidator = ! metadata.etag.empty() && metadata.etag.substr (0, 2) != "W/"
                ? header (response, "etag") : header (response, "last-modified");
            if (currentValidator != metadata.validator()) return reject ("Download entity changed; restart the download");
            expectedTotal = range->total;
            if (expectedBody && *expectedBody != range->last - range->first + 1)
                return reject ("Download content length disagrees with its resume range");
            expectedBody = range->last - range->first + 1;
        }
        else expectedTotal = expectedBody;
        if ((expectedTotal && *expectedTotal > request.maxBytes)
            || (expectedBody && *expectedBody > request.maxBytes)) return reject ("Download exceeds the size limit");
        if (response.statusCode == 200)
        {
            if (! rewindFile (file.get(), true)) return reject ("Cannot restart download part");
            crypto_hash_sha256_init (&hash);
            result.bytes = 0;
        }
        metadata = { *url, digest, response.url, header (response, "etag"), header (response, "last-modified"), {}, {} };
        if (! writeMetadata (sidecar, metadata)) return reject ("Cannot save download resume information");
        accepted = true;
        return true;
    }, [&] (std::string_view chunk)
    {
        if (! accepted) { failure = "Download body arrived before validation"; return false; }
        if (chunk.size() > request.maxBytes - result.bytes
            || (expectedBody && chunk.size() > *expectedBody - received))
        { failure = "Download exceeds the size limit or declared length"; return false; }
        if (std::fwrite (chunk.data(), 1, chunk.size(), file.get()) != chunk.size())
        { failure = "Cannot write download part"; return false; }
        crypto_hash_sha256_update (&hash, reinterpret_cast<const unsigned char*> (chunk.data()), chunk.size());
        result.bytes += chunk.size();
        received += chunk.size();
        if (control.progress) control.progress ({ result.bytes, expectedTotal.value_or (0), 0 });
        return true;
    }, control, resume ? metadata.effectiveUrl : std::string{});
    result.status = operation.status;
    result.error = failure.empty() ? operation.error : failure;
    if (! failure.empty()) result.status = OperationStatus::Failed;
    if (std::fflush (file.get()) != 0) return fail ("Cannot flush download part");
    if (! result) return result;
    if (! accepted || (expectedBody && received != *expectedBody)
        || (expectedTotal && result.bytes != *expectedTotal)) return fail ("Truncated download");
    result.sha256 = hashDigest (hash);
    if (! digest.empty() && result.sha256 != digest) return fail ("Download SHA-256 does not match the expected digest");
    metadata.completedBytes = result.bytes;
    metadata.actualDigest = result.sha256;
    if (! writeMetadata (sidecar, metadata)) return fail ("Cannot save completed download information");
    return result;
}
} // namespace duskstudio::sfz

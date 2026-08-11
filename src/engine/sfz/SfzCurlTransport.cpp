#include "SfzCurlTransport.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

namespace duskstudio::sfz
{
namespace
{
constexpr char kUserAgent[] = "DuskStudio";

void initialiseCurlOnce()
{
    static std::once_flag flag;
    std::call_once (flag, [] { curl_global_init (CURL_GLOBAL_DEFAULT); });
}

struct TransferState
{
    const TransferRequest* request { nullptr };
    const TransferCallbacks* callbacks { nullptr };

    long responseStatus { 0 };
    std::uint64_t declaredTotalBytes { 0 };
    bool responseDelivered { false };
    bool resumeAccepted { false };

    std::uint64_t receivedBytes { 0 };
    bool cancelled { false };
    bool limitExceeded { false };
    std::string error;
};

std::string trimHeaderValue (const char* data, std::size_t bytes)
{
    std::size_t begin = 0;
    while (begin < bytes && (data[begin] == ' ' || data[begin] == '\t'))
        ++begin;
    std::size_t end = bytes;
    while (end > begin
           && (data[end - 1] == '\r' || data[end - 1] == '\n'
               || data[end - 1] == ' ' || data[end - 1] == '\t'))
        --end;
    return std::string (data + begin, end - begin);
}

bool parseUnsigned (const std::string& value, std::uint64_t& out)
{
    if (value.empty())
        return false;

    std::uint64_t parsed = 0;
    for (const auto c : value)
    {
        if (c < '0' || c > '9')
            return false;
        const auto digit = static_cast<std::uint64_t> (c - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
            return false;
        parsed = parsed * 10U + digit;
    }
    out = parsed;
    return true;
}

// "bytes 100-199/1234" - the value after the slash is the full resource size,
// which is what the caller budgets against. "*" means the server declined to
// say, so the size stays unknown.
bool parseContentRangeTotal (const std::string& value, std::uint64_t& out)
{
    const auto slash = value.rfind ('/');
    if (slash == std::string::npos)
        return false;
    return parseUnsigned (value.substr (slash + 1), out);
}

std::size_t onHeader (char* buffer, std::size_t size, std::size_t items, void* userData)
{
    auto& state = *static_cast<TransferState*> (userData);
    const auto bytes = size * items;

    // A redirect chain delivers one header block per hop; only the final one
    // describes the body, so every status line resets what was collected.
    if (bytes >= 5 && std::memcmp (buffer, "HTTP/", 5) == 0)
    {
        state.responseStatus = 0;
        state.declaredTotalBytes = 0;
        const std::string line (buffer, bytes);
        const auto space = line.find (' ');
        if (space != std::string::npos)
        {
            std::uint64_t status = 0;
            if (parseUnsigned (trimHeaderValue (line.data() + space + 1,
                                                std::min<std::size_t> (3, line.size() - space - 1)),
                               status))
                state.responseStatus = static_cast<long> (status);
        }
        return bytes;
    }

    const auto colon = static_cast<const char*> (std::memchr (buffer, ':', bytes));
    if (colon == nullptr)
        return bytes;

    const std::size_t nameLength = static_cast<std::size_t> (colon - buffer);
    const auto name = trimHeaderValue (buffer, nameLength);
    const auto value = trimHeaderValue (colon + 1, bytes - nameLength - 1);

    const auto matches = [&name] (const char* expected)
    {
        const std::size_t length = std::strlen (expected);
        if (name.size() != length)
            return false;
        for (std::size_t i = 0; i < length; ++i)
        {
            auto c = static_cast<unsigned char> (name[i]);
            if (c >= 'A' && c <= 'Z')
                c = static_cast<unsigned char> (c - 'A' + 'a');
            if (c != static_cast<unsigned char> (expected[i]))
                return false;
        }
        return true;
    };

    if (matches ("content-range"))
    {
        std::uint64_t total = 0;
        if (parseContentRangeTotal (value, total))
            state.declaredTotalBytes = total;
    }
    else if (matches ("content-length") && state.declaredTotalBytes == 0)
    {
        std::uint64_t length = 0;
        if (parseUnsigned (value, length))
            state.declaredTotalBytes = state.responseStatus == 206
                                           ? state.request->resumeOffset + length
                                           : length;
    }
    return bytes;
}

std::size_t onWrite (char* data, std::size_t size, std::size_t items, void* userData)
{
    auto& state = *static_cast<TransferState*> (userData);
    const auto bytes = size * items;

    if (! state.responseDelivered)
    {
        state.responseDelivered = true;
        state.resumeAccepted = state.request->resumeOffset > 0
                               && state.responseStatus == 206;

        const auto baseOffset = state.resumeAccepted ? state.request->resumeOffset : 0;
        if (state.declaredTotalBytes > state.request->limits.maximumBytes)
        {
            state.limitExceeded = true;
            state.error = "the server declared more bytes than the catalog allows";
            return 0;
        }
        if (baseOffset > state.request->limits.maximumBytes)
        {
            state.limitExceeded = true;
            state.error = "the resume offset is past the allowed size";
            return 0;
        }

        if (state.callbacks->onResponse
            && ! state.callbacks->onResponse (state.resumeAccepted, state.declaredTotalBytes))
        {
            state.cancelled = true;
            return 0;
        }
    }

    const auto baseOffset = state.resumeAccepted ? state.request->resumeOffset : 0;
    if (bytes > state.request->limits.maximumBytes - baseOffset - state.receivedBytes)
    {
        state.limitExceeded = true;
        state.error = "the response exceeded the allowed archive size";
        return 0;
    }

    if (state.callbacks->onData
        && ! state.callbacks->onData (reinterpret_cast<const unsigned char*> (data), bytes))
    {
        state.cancelled = true;
        return 0;
    }

    state.receivedBytes += bytes;
    return bytes;
}

int onProgress (void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto& state = *static_cast<TransferState*> (userData);
    if (state.callbacks->onProgress == nullptr)
        return 0;

    const auto baseOffset = state.resumeAccepted ? state.request->resumeOffset : 0;
    if (! state.callbacks->onProgress (baseOffset + state.receivedBytes,
                                       state.declaredTotalBytes))
    {
        state.cancelled = true;
        return 1;
    }
    return 0;
}
} // namespace

CurlTransport::CurlTransport()
{
    initialiseCurlOnce();
}

CurlTransport::~CurlTransport() = default;

TransferResult CurlTransport::fetch (const TransferRequest& request,
                                     const TransferCallbacks& callbacks)
{
    TransferResult result;
    if (request.url.compare (0, 8, "https://") != 0)
    {
        result.error = "the download URL must use HTTPS";
        return result;
    }
    if (request.limits.maximumBytes == 0)
    {
        result.error = "no download size ceiling was configured";
        return result;
    }

    auto* handle = curl_easy_init();
    if (handle == nullptr)
    {
        result.error = "the network transport could not be created";
        return result;
    }

    TransferState state;
    state.request = &request;
    state.callbacks = &callbacks;

    curl_easy_setopt (handle, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt (handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt (handle, CURLOPT_MAXREDIRS,
                      static_cast<long> (request.limits.maximumRedirects));
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt (handle, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt (handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt (handle, CURLOPT_PROTOCOLS, static_cast<long> (CURLPROTO_HTTPS));
    curl_easy_setopt (handle, CURLOPT_REDIR_PROTOCOLS, static_cast<long> (CURLPROTO_HTTPS));
#endif
    curl_easy_setopt (handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt (handle, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt (handle, CURLOPT_USERAGENT, kUserAgent);
    // Signals are process-wide state and this runs off the message thread.
    curl_easy_setopt (handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt (handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt (handle, CURLOPT_CONNECTTIMEOUT,
                      static_cast<long> (request.limits.connectTimeoutSeconds));
    curl_easy_setopt (handle, CURLOPT_TIMEOUT,
                      static_cast<long> (request.limits.overallTimeoutSeconds));
    curl_easy_setopt (handle, CURLOPT_LOW_SPEED_LIMIT,
                      static_cast<long> (request.limits.stallBytesPerSecond));
    curl_easy_setopt (handle, CURLOPT_LOW_SPEED_TIME,
                      static_cast<long> (request.limits.stallTimeoutSeconds));
    // Content negotiation is deliberately left off: a compressed transfer
    // encoding would make the streamed byte count disagree with the size the
    // catalog signed, and with the byte range a resume asks for.
    curl_easy_setopt (handle, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt (handle, CURLOPT_HEADERDATA, &state);
    curl_easy_setopt (handle, CURLOPT_WRITEFUNCTION, onWrite);
    curl_easy_setopt (handle, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt (handle, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt (handle, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt (handle, CURLOPT_NOPROGRESS, 0L);
    if (request.resumeOffset > 0)
        curl_easy_setopt (handle, CURLOPT_RESUME_FROM_LARGE,
                          static_cast<curl_off_t> (request.resumeOffset));

    std::array<char, CURL_ERROR_SIZE> errorBuffer {};
    curl_easy_setopt (handle, CURLOPT_ERRORBUFFER, errorBuffer.data());

    const auto code = curl_easy_perform (handle);
    curl_easy_cleanup (handle);

    result.receivedBytes = state.receivedBytes;
    if (state.cancelled)
    {
        result.status = TransferStatus::cancelled;
        return result;
    }
    if (state.limitExceeded)
    {
        result.error = state.error;
        return result;
    }
    if (code != CURLE_OK)
    {
        const auto* detail = errorBuffer[0] != '\0' ? errorBuffer.data()
                                                    : curl_easy_strerror (code);
        result.error = std::string ("the download failed: ") + detail;
        return result;
    }
    if (! state.responseDelivered)
    {
        // A zero-length body never reaches the write callback, so the response
        // contract still has to be honoured before reporting completion.
        state.resumeAccepted = request.resumeOffset > 0 && state.responseStatus == 206;
        if (callbacks.onResponse
            && ! callbacks.onResponse (state.resumeAccepted, state.declaredTotalBytes))
        {
            result.status = TransferStatus::cancelled;
            return result;
        }
    }

    result.status = TransferStatus::completed;
    return result;
}
} // namespace duskstudio::sfz

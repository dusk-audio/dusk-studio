#include "SfzCurlTransport.h"
#include "SfzHttpHeaders.h"

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
    std::uint64_t contentRangeStart { 0 };
    bool contentRangeStartKnown { false };
    bool responseDelivered { false };
    bool resumeAccepted { false };

    std::uint64_t receivedBytes { 0 };
    bool cancelled { false };
    bool limitExceeded { false };
    bool callbackThrew { false };
    std::string error;
};

// A user std::function reached from a libcurl C callback frame must never let an
// exception unwind through libcurl - that is undefined behaviour. Any throw is
// caught here and turned into an abort of the transfer; the false return is the
// signal every caller already uses to stop.
template <typename Fn>
bool invokeGuarded (TransferState& state, Fn&& fn)
{
    try
    {
        return fn();
    }
    catch (...)
    {
        state.callbackThrew = true;
        if (state.error.empty())
            state.error = "a transfer callback failed";
        return false;
    }
}

std::size_t onHeader (char* buffer, std::size_t size, std::size_t items, void* userData)
{
    auto& state = *static_cast<TransferState*> (userData);
    const auto bytes = size * items;

    long status = 0;
    if (http::parseHttpStatus (buffer, bytes, status))
    {
        state.responseStatus = status;
        state.declaredTotalBytes = 0;
        state.contentRangeStart = 0;
        state.contentRangeStartKnown = false;
        return bytes;
    }

    const auto colon = static_cast<const char*> (std::memchr (buffer, ':', bytes));
    if (colon == nullptr)
        return bytes;

    const std::size_t nameLength = static_cast<std::size_t> (colon - buffer);
    const auto name = http::trimHeaderValue (buffer, nameLength);
    const auto value = http::trimHeaderValue (colon + 1, bytes - nameLength - 1);

    if (http::headerNameMatches (name, "content-range"))
    {
        std::uint64_t total = 0;
        if (http::parseContentRangeTotal (value, total))
            state.declaredTotalBytes = total;
        std::uint64_t start = 0;
        if (http::parseContentRangeStart (value, start))
        {
            state.contentRangeStart = start;
            state.contentRangeStartKnown = true;
        }
    }
    else if (http::headerNameMatches (name, "content-length") && state.declaredTotalBytes == 0)
    {
        std::uint64_t length = 0;
        if (http::parseUnsigned (value, length))
        {
            if (state.responseStatus == 206)
            {
                // resumeOffset + length can wrap past 64 bits; an overflowing
                // total is not a number the caller can budget against, so leave
                // it unknown rather than store a small wrapped value.
                if (length
                    <= std::numeric_limits<std::uint64_t>::max() - state.request->resumeOffset)
                    state.declaredTotalBytes = state.request->resumeOffset + length;
            }
            else
            {
                state.declaredTotalBytes = length;
            }
        }
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
        // A 206 alone is not proof the resume took: the server must report that
        // the returned bytes start exactly where the request asked. A 206 whose
        // Content-Range begins at 0 (or omits the start) is a whole fresh
        // resource that would corrupt the part file if appended, so the sink is
        // told the resume was refused and truncates.
        state.resumeAccepted = state.request->resumeOffset > 0
                               && state.responseStatus == 206
                               && state.contentRangeStartKnown
                               && state.contentRangeStart == state.request->resumeOffset;

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

        if (state.callbacks->onResponse)
        {
            const bool ok = invokeGuarded (state, [&]
            {
                return state.callbacks->onResponse (state.resumeAccepted,
                                                    state.declaredTotalBytes);
            });
            if (! ok)
            {
                if (! state.callbackThrew)
                    state.cancelled = true;
                return 0;
            }
        }
    }

    const auto baseOffset = state.resumeAccepted ? state.request->resumeOffset : 0;
    if (bytes > state.request->limits.maximumBytes - baseOffset - state.receivedBytes)
    {
        state.limitExceeded = true;
        state.error = "the response exceeded the allowed archive size";
        return 0;
    }

    if (state.callbacks->onData)
    {
        const bool ok = invokeGuarded (state, [&]
        {
            return state.callbacks->onData (reinterpret_cast<const unsigned char*> (data), bytes);
        });
        if (! ok)
        {
            if (! state.callbackThrew)
                state.cancelled = true;
            return 0;
        }
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
    const bool ok = invokeGuarded (state, [&]
    {
        return state.callbacks->onProgress (baseOffset + state.receivedBytes,
                                            state.declaredTotalBytes);
    });
    if (! ok)
    {
        if (! state.callbackThrew)
            state.cancelled = true;
        return 1;
    }
    return 0;
}

// On LLP64 (Windows) long is 32-bit, so an unsigned past LONG_MAX would wrap to a
// negative value libcurl reads as "no limit" - CURLOPT_MAXREDIRS of UINT_MAX
// becoming -1 is unbounded redirects. Clamp before the cast.
long toCurlLong (unsigned value) noexcept
{
    constexpr auto ceiling = static_cast<unsigned long> (std::numeric_limits<long>::max());
    return static_cast<long> (std::min<unsigned long> (value, ceiling));
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
    curl_easy_setopt (handle, CURLOPT_MAXREDIRS, toCurlLong (request.limits.maximumRedirects));

    // The protocol allowlist and peer verification are what keep a redirect from
    // walking off HTTPS and what authenticate the server. If the running libcurl
    // is older than its headers and rejects one of these - CURLOPT_PROTOCOLS_STR
    // against a pre-7.85 shared object returns CURLE_UNKNOWN_OPTION - the pin is
    // silently not applied, so any rejection fails the whole transfer closed.
    bool securedOk = true;
    const auto secured = [&securedOk] (CURLcode code)
    {
        if (code != CURLE_OK)
            securedOk = false;
    };
#if LIBCURL_VERSION_NUM >= 0x075500
    secured (curl_easy_setopt (handle, CURLOPT_PROTOCOLS_STR, "https"));
    secured (curl_easy_setopt (handle, CURLOPT_REDIR_PROTOCOLS_STR, "https"));
#else
    secured (curl_easy_setopt (handle, CURLOPT_PROTOCOLS, static_cast<long> (CURLPROTO_HTTPS)));
    secured (curl_easy_setopt (handle, CURLOPT_REDIR_PROTOCOLS, static_cast<long> (CURLPROTO_HTTPS)));
#endif
    secured (curl_easy_setopt (handle, CURLOPT_SSL_VERIFYPEER, 1L));
    secured (curl_easy_setopt (handle, CURLOPT_SSL_VERIFYHOST, 2L));
    if (! securedOk)
    {
        curl_easy_cleanup (handle);
        result.error = "the transport could not be secured";
        return result;
    }

    curl_easy_setopt (handle, CURLOPT_USERAGENT, kUserAgent);
    // CURLOPT_PROXY is deliberately left at its default (libcurl honours the
    // environment proxy): TLS is verified end to end so a proxy cannot MITM the
    // transfer, and corporate networks require the outbound proxy to be used.
    // Signals are process-wide state and this runs off the message thread.
    curl_easy_setopt (handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt (handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt (handle, CURLOPT_CONNECTTIMEOUT,
                      toCurlLong (request.limits.connectTimeoutSeconds));
    curl_easy_setopt (handle, CURLOPT_TIMEOUT,
                      toCurlLong (request.limits.overallTimeoutSeconds));
    curl_easy_setopt (handle, CURLOPT_LOW_SPEED_LIMIT,
                      toCurlLong (request.limits.stallBytesPerSecond));
    curl_easy_setopt (handle, CURLOPT_LOW_SPEED_TIME,
                      toCurlLong (request.limits.stallTimeoutSeconds));
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
    if (state.callbackThrew)
    {
        result.error = state.error;
        return result;
    }
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
        state.resumeAccepted = request.resumeOffset > 0
                               && state.responseStatus == 206
                               && state.contentRangeStartKnown
                               && state.contentRangeStart == request.resumeOffset;
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

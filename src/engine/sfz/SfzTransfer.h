#pragma once

#include "SfzOperation.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace duskstudio::sfz
{
using HttpHeaders = std::map<std::string, std::string>;

struct HttpRequest
{
    std::string url;
    HttpHeaders headers;
    long connectTimeoutSeconds = 15;
    long timeoutSeconds = 60;
    long lowSpeedBytesPerSecond = 128;
    long lowSpeedSeconds = 30;
};

struct HttpResponse
{
    int statusCode = 0;
    std::string url;
    HttpHeaders headers;
};

using HttpHeadersCallback = std::function<bool (const HttpResponse&)>;
using HttpBodyCallback = std::function<bool (std::string_view)>;

class HttpTransport
{
public:
    virtual ~HttpTransport() = default;
    // One response: deliver headers before bounded body chunks, abort when either callback returns false.
    virtual OperationResult perform (const HttpRequest&, const HttpHeadersCallback&,
                                     const HttpBodyCallback&, const OperationControl&) = 0;
};

class CurlTransport final : public HttpTransport
{
public:
    OperationResult perform (const HttpRequest&, const HttpHeadersCallback&,
                             const HttpBodyCallback&, const OperationControl&) override;
};

struct HttpTextResult : OperationResult
{
    HttpResponse response;
    std::string body;
};

HttpTextResult fetchHttpText (HttpTransport&, const HttpRequest&, std::uint64_t maxBytes,
                              const OperationControl& = {});
HttpTextResult fetchHttpText (HttpTransport&, const std::string& url, std::uint64_t maxBytes,
                              const OperationControl& = {});

struct DownloadRequest
{
    std::string url;
    std::filesystem::path partPath;
    std::string expectedSha256;
    std::uint64_t maxBytes = 2ULL * 1024 * 1024 * 1024;
};

struct DownloadResult : OperationResult
{
    std::string sha256;
    std::uint64_t bytes = 0;
};

DownloadResult downloadFile (HttpTransport&, const DownloadRequest&,
                              const OperationControl& = {});
} // namespace duskstudio::sfz

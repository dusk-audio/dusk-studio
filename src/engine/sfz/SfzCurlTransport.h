#pragma once

#include "SfzTransport.h"

namespace duskstudio::sfz
{
// HTTPS-only libcurl transport. Redirects are followed but never leave HTTPS,
// the response body is streamed straight to the caller, and the callbacks are
// the only cancellation and progress path. Instances are not thread-safe; give
// each worker its own.
class CurlTransport final : public Transport
{
public:
    CurlTransport();
    ~CurlTransport() override;

    CurlTransport (const CurlTransport&) = delete;
    CurlTransport& operator= (const CurlTransport&) = delete;

    TransferResult fetch (const TransferRequest& request,
                          const TransferCallbacks& callbacks) override;
};
} // namespace duskstudio::sfz
